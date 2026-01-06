#include "core/http_data_forwarder.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <algorithm>
#include <cstring>
#include <sstream>
#include "http/http_handler.h"
#include "http/http_request.h"
#include "http/http_response.h"

namespace lb::core {

HttpDataForwarder::HttpDataForwarder(net::EpollReactor& reactor,
                                     std::function<void(int)> start_backpressure,
                                     std::function<void(int)> clear_backpressure,
                                     std::function<void(int)> close_connection,
                                     std::function<std::string(int)> get_client_ip, bool is_https)
    : reactor_(reactor), start_backpressure_(std::move(start_backpressure)),
      clear_backpressure_(std::move(clear_backpressure)),
      close_connection_(std::move(close_connection)), get_client_ip_(std::move(get_client_ip)),
      is_https_(is_https) {}

void HttpDataForwarder::forward(net::Connection* from, net::Connection* to) {
    if (!from || !to)
        return;

    if (from->state() != net::ConnectionState::ESTABLISHED ||
        to->state() != net::ConnectionState::ESTABLISHED)
        return;

    auto& read_buf = from->read_buffer();
    if (read_buf.empty())
        return;

    int from_fd = from->fd();

    bool is_http_request = lb::http::HttpHandler::is_http_request(read_buf);

    if (is_http_request) {
        auto& state = http_states_[from_fd];

        if (!state.request_parsed_) {
            if (parse_and_modify_http_request(from, from_fd)) {
                state.request_parsed_ = true;
            } else {
                return;
            }
        }

        if (state.request_parsed_ && state.parser.has_error()) {
            lb::http::HttpResponse error_resp =
                lb::http::HttpResponse::bad_request("Invalid HTTP request");
            auto error_bytes = error_resp.to_bytes();
            to->write_buffer().assign(error_bytes.begin(), error_bytes.end());
            reactor_.mod_fd(to->fd(), EPOLLIN | EPOLLOUT);
            read_buf.clear();
            state.request_parsed_ = false;
            state.parser.reset();
            return;
        }
    }

    size_t available = to->write_available();
    if (available == 0) {
        start_backpressure_(from_fd);
        reactor_.mod_fd(from_fd, EPOLLOUT);
        return;
    }

    clear_backpressure_(from_fd);

    size_t to_copy = std::min(read_buf.size(), available);
    to->write_buffer().insert(to->write_buffer().end(), read_buf.begin(),
                              read_buf.begin() + to_copy);
    read_buf.erase(read_buf.begin(), read_buf.begin() + to_copy);

    reactor_.mod_fd(to->fd(), EPOLLIN | EPOLLOUT);
    reactor_.mod_fd(from_fd, EPOLLIN | EPOLLOUT);

    if (!to->write_to_fd()) {
        close_connection_(to->fd());
        return;
    }
}

bool HttpDataForwarder::parse_and_modify_http_request(net::Connection* conn, int fd) {
    auto& read_buf = conn->read_buffer();

    if (!lb::http::HttpHandler::is_http_request(read_buf))
        return false;

    auto& state = http_states_[fd];

    size_t consumed = 0;
    if (state.parser.parse(read_buf, consumed)) {
        auto& request = const_cast<lb::http::HttpRequest&>(state.parser.request());
        std::string client_ip = get_client_ip_(fd);
        if (client_ip.empty())
            client_ip = lb::http::HttpHandler::extract_client_ip(fd);

        lb::http::HttpHandler::modify_request_headers(request, client_ip, is_https_);

        auto modified_request = serialize_http_request(request);

        read_buf = std::move(modified_request);

        return true;
    }

    return false;
}

std::vector<uint8_t> HttpDataForwarder::serialize_http_request(
    const lb::http::HttpRequest& request) {
    std::ostringstream oss;

    oss << lb::http::method_to_string(request.method) << " " << request.path;
    if (!request.query_string.empty())
        oss << "?" << request.query_string;
    oss << " " << lb::http::version_to_string(request.version) << "\r\n";

    for (const auto& [name, value] : request.headers) {
        oss << name << ": " << value << "\r\n";
    }

    oss << "\r\n";

    if (!request.body.empty()) {
        oss.write(reinterpret_cast<const char*>(request.body.data()), request.body.size());
    }

    std::string str = oss.str();
    return std::vector<uint8_t>(str.begin(), str.end());
}

} // namespace lb::core
