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
#include "logging/logger.h"
#include "metrics/metrics.h"

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
            lb::metrics::Metrics::instance().increment_bad_requests();

            std::string client_ip = get_client_ip_(from_fd);
            if (client_ip.empty())
                client_ip = "unknown";

            std::string error_msg = state.parser.error_message();
            if (error_msg.empty())
                error_msg = "Invalid HTTP request";

            lb::logging::Logger::instance().warn("Invalid HTTP request from " + client_ip +
                                                 " (fd=" + std::to_string(from_fd) +
                                                 "): " + error_msg);

            if (access_log_callback_) {
                RequestInfo bad_req;
                bad_req.client_ip = client_ip;
                bad_req.method = "INVALID";
                bad_req.path = "";
                bad_req.status_code = 400;
                bad_req.start_time = std::chrono::system_clock::now();
                bad_req.response_complete_time = bad_req.start_time;
                bad_req.response_complete = true;
                bad_req.bytes_received = read_buf.size();
                access_log_callback_(from_fd, bad_req);
            }

            lb::http::HttpResponse error_resp =
                lb::http::HttpResponse::bad_request("Invalid HTTP request");
            auto error_bytes = error_resp.to_bytes();
            size_t old_sz = from->write_buffer().size();
            size_t new_sz = error_bytes.size();
            if (new_sz > old_sz) {
                if (!from->try_reserve_additional_bytes(new_sz - old_sz)) {
                    close_connection_(from_fd);
                    if (to) {
                        close_connection_(to->fd());
                    }
                    return;
                }
            } else if (old_sz > new_sz) {
                from->release_accounted_bytes(old_sz - new_sz);
            }
            from->write_buffer().assign(error_bytes.begin(), error_bytes.end());
            reactor_.mod_fd(from_fd, EPOLLIN | EPOLLOUT);
            read_buf.clear();
            state.request_parsed_ = false;
            state.parser.reset();
            if (to) {
                close_connection_(to->fd());
            }
            return;
        }
    } else {
        int client_fd = get_client_fd_ ? get_client_fd_(from_fd) : 0;
        if (client_fd > 0) {
            auto& state = http_states_[client_fd];

            if (!state.response_parsed_) {
                if (parse_and_modify_http_response(from, client_fd)) {
                    state.response_parsed_ = true;
                } else {
                    return;
                }
            }

            if (state.request_info.status_code == 0) {
                parse_response_status(client_fd, read_buf);
            }
        }
    }

    size_t available = to->write_available();
    size_t to_copy = std::min(read_buf.size(), available);
    if (to_copy == 0) {
        start_backpressure_(from_fd);
        reactor_.mod_fd(from_fd, EPOLLOUT);
        return;
    }

    bool was_buffer_full = from->buffer_full();
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

    if (was_buffer_full && from->has_read_space()) {
        from->set_buffer_full(false);
    }

    reactor_.mod_fd(to->fd(), EPOLLIN | EPOLLOUT);

    uint32_t from_events = EPOLLOUT;
    if (!from->memory_blocked() && !from->buffer_full()) {
        from_events |= EPOLLIN;
    }
    reactor_.mod_fd(from_fd, from_events);

    if (!to->write_to_fd()) {
        close_connection_(to->fd());
        return;
    }

    auto has_pending_send = [](net::Connection* conn) {
        return conn && (!conn->write_buffer().empty() || conn->pending_kernel_bytes() > 0);
    };
    if (has_pending_send(to)) {
        start_backpressure_(from_fd);
    } else {
        clear_backpressure_(from_fd);
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
                        it->second.response_parsed_ = false;
                        it->second.parser.reset();
                        it->second.response_parser.reset();
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

        if (session_key_update_callback_ && !sticky_session_cookie_name_.empty()) {
            std::string cookie_value = request.get_cookie(sticky_session_cookie_name_);
            if (!cookie_value.empty()) {
                session_key_update_callback_(fd, cookie_value);
            }
        }

        auto modified_request = serialize_http_request(request);

        size_t before = read_buf.size();
        size_t after = modified_request.size();
        if (after > before) {
            if (!conn->try_reserve_additional_bytes(after - before)) {
                start_backpressure_(fd);
                reactor_.mod_fd(fd, EPOLLOUT);
                return false;
            }
        } else if (before > after) {
            conn->release_accounted_bytes(before - after);
        }
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

void HttpDataForwarder::set_custom_response_header_modifier(
    std::function<void(lb::http::ParsedHttpResponse&)> modifier) {
    if (modifier) {
        custom_response_header_modifier_ = std::move(modifier);
    } else {
        custom_response_header_modifier_ = nullptr;
    }
}

void HttpDataForwarder::set_session_key_update_callback(SessionKeyUpdateCallback callback,
                                                        const std::string& cookie_name) {
    session_key_update_callback_ = std::move(callback);
    sticky_session_cookie_name_ = cookie_name;
}

void HttpDataForwarder::set_cookie_injection_callback(CookieInjectionCallback callback) {
    cookie_injection_callback_ = std::move(callback);
}

bool HttpDataForwarder::parse_and_modify_http_response(net::Connection* conn, int client_fd) {
    auto& read_buf = conn->read_buffer();
    auto& state = http_states_[client_fd];

    size_t consumed = 0;
    if (state.response_parser.parse(read_buf, consumed)) {
        auto& response =
            const_cast<lb::http::ParsedHttpResponse&>(state.response_parser.response());

        if (custom_response_header_modifier_) {
            custom_response_header_modifier_(response);
        }

        if (cookie_injection_callback_) {
            std::string cookie_value = cookie_injection_callback_(client_fd);
            if (!cookie_value.empty()) {
                response.headers["Set-Cookie"] = cookie_value;
            }
        }

        auto modified_response = serialize_http_response(response);

        size_t before = read_buf.size();
        size_t after = modified_response.size();
        if (after > before) {
            if (!conn->try_reserve_additional_bytes(after - before)) {
                start_backpressure_(conn->fd());
                reactor_.mod_fd(conn->fd(), EPOLLOUT);
                return false;
            }
        } else if (before > after) {
            conn->release_accounted_bytes(before - after);
        }
        read_buf = std::move(modified_response);

        return true;
    }

    return false;
}

std::vector<uint8_t> HttpDataForwarder::serialize_http_response(
    const lb::http::ParsedHttpResponse& response) {
    std::ostringstream oss;

    oss << "HTTP/" << lb::http::version_to_string(response.version) << " " << response.status_code
        << " " << response.reason_phrase << "\r\n";

    for (const auto& [name, value] : response.headers) {
        oss << name << ": " << value << "\r\n";
    }

    oss << "\r\n";

    if (!response.body.empty()) {
        oss.write(reinterpret_cast<const char*>(response.body.data()), response.body.size());
    }

    std::string str = oss.str();
    return std::vector<uint8_t>(str.begin(), str.end());
}

} // namespace lb::core
