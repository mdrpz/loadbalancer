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
    } else {
        int client_fd = get_client_fd_ ? get_client_fd_(from_fd) : 0;
        if (client_fd > 0) {
            auto it = http_states_.find(client_fd);
            if (it != http_states_.end() && it->second.request_info.status_code == 0) {
                if (read_buf.size() >= 12 && read_buf[0] == 'H' && read_buf[1] == 'T' &&
                    read_buf[2] == 'T' && read_buf[3] == 'P') {
                    parse_response_status(client_fd, read_buf);
                }
            }
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

    if (is_http_request) {
        auto it = http_states_.find(from_fd);
        if (it != http_states_.end()) {
            it->second.request_info.bytes_received += to_copy;
        }
    } else {
        int client_fd = get_client_fd_ ? get_client_fd_(from_fd) : 0;
        if (client_fd > 0) {
            auto it = http_states_.find(client_fd);
            if (it != http_states_.end()) {
                it->second.request_info.bytes_sent += to_copy;
            }
        }
    }

    read_buf.erase(read_buf.begin(), read_buf.begin() + to_copy);

    reactor_.mod_fd(to->fd(), EPOLLIN | EPOLLOUT);
    reactor_.mod_fd(from_fd, EPOLLIN | EPOLLOUT);

    if (!to->write_to_fd()) {
        close_connection_(to->fd());
        return;
    }

    if (!is_http_request) {
        int client_fd = get_client_fd_ ? get_client_fd_(from_fd) : 0;
        if (client_fd > 0) {
            auto it = http_states_.find(client_fd);
            if (it != http_states_.end() && it->second.request_info.status_code > 0) {
                if (read_buf.empty() && !it->second.request_info.response_complete) {
                    it->second.request_info.response_complete_time =
                        std::chrono::system_clock::now();
                    it->second.request_info.response_complete = true;

                    if (access_log_callback_) {
                        access_log_callback_(client_fd, it->second.request_info);

                        it->second.request_info = RequestInfo{};
                        it->second.request_parsed_ = false;
                        it->second.parser.reset();
                    }
                }
            }
        }
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
            client_ip = "unknown";

        state.request_info.method = lb::http::method_to_string(request.method);
        state.request_info.path = request.path;
        if (!request.query_string.empty())
            state.request_info.path += "?" + request.query_string;
        state.request_info.client_ip = client_ip;
        state.request_info.start_time = std::chrono::system_clock::now();

        lb::http::HttpHandler::modify_request_headers(request, client_ip, is_https_);

        if (custom_header_modifier_) {
            custom_header_modifier_(request);
        }

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

RequestInfo* HttpDataForwarder::get_request_info(int client_fd) {
    auto it = http_states_.find(client_fd);
    if (it != http_states_.end() && !it->second.request_info.method.empty()) {
        return &it->second.request_info;
    }
    return nullptr;
}

void HttpDataForwarder::set_backend_for_request(int client_fd, const std::string& backend) {
    auto it = http_states_.find(client_fd);
    if (it != http_states_.end()) {
        it->second.request_info.backend = backend;
    }
}

void HttpDataForwarder::parse_response_status(int client_fd,
                                              const std::vector<uint8_t>& response_data) {
    auto it = http_states_.find(client_fd);
    if (it == http_states_.end())
        return;

    if (it->second.request_info.status_code > 0)
        return;

    if (response_data.size() < 12)
        return;

    for (size_t i = 0; i < response_data.size() - 8; ++i) {
        if (response_data[i] == 'H' && response_data[i + 1] == 'T' && response_data[i + 2] == 'T' &&
            response_data[i + 3] == 'P' && response_data[i + 4] == '/' &&
            (response_data[i + 5] == '1' || response_data[i + 5] == '2') &&
            response_data[i + 6] == '.' &&
            (response_data[i + 7] == '0' || response_data[i + 7] == '1') &&
            response_data[i + 8] == ' ') {

            size_t status_start = i + 9;
            if (status_start + 3 < response_data.size()) {
                int status = 0;
                for (int j = 0; j < 3 && status_start + j < response_data.size(); ++j) {
                    char c = response_data[status_start + j];
                    if (c >= '0' && c <= '9') {
                        status = status * 10 + (c - '0');
                    } else {
                        break;
                    }
                }

                if (status >= 100 && status < 600) {
                    it->second.request_info.status_code = status;
                    return;
                }
            }
            break;
        }
    }
}

void HttpDataForwarder::clear_request_info(int client_fd) {
    http_states_.erase(client_fd);
}

void HttpDataForwarder::set_backend_to_client_map(std::function<int(int)> get_client_fd) {
    get_client_fd_ = std::move(get_client_fd);
}

void HttpDataForwarder::set_access_log_callback(AccessLogCallback callback) {
    access_log_callback_ = std::move(callback);
}

void HttpDataForwarder::set_custom_header_modifier(
    std::function<void(lb::http::HttpRequest&)> modifier) {
    if (modifier) {
        custom_header_modifier_ = std::move(modifier);
    } else {
        custom_header_modifier_ = nullptr;
    }
}

} // namespace lb::core
