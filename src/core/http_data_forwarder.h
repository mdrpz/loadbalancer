#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include "http/http_handler.h"
#include "http/http_request.h"
#include "net/connection.h"
#include "net/epoll_reactor.h"

namespace lb::core {

struct RequestInfo {
    std::string method;
    std::string path;
    std::string client_ip;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point response_complete_time;
    int status_code{0};
    std::string backend;
    bool response_complete{false};
    size_t bytes_sent{0};
    size_t bytes_received{0};
};

class HttpDataForwarder {
public:
    HttpDataForwarder(net::EpollReactor& reactor, std::function<void(int)> start_backpressure,
                      std::function<void(int)> clear_backpressure,
                      std::function<void(int)> close_connection,
                      std::function<std::string(int)> get_client_ip, bool is_https);

    void forward(net::Connection* from, net::Connection* to);

    RequestInfo* get_request_info(int client_fd);
    void set_backend_for_request(int client_fd, const std::string& backend);
    void parse_response_status(int client_fd, const std::vector<uint8_t>& response_data);
    void clear_request_info(int client_fd);
    void set_backend_to_client_map(std::function<int(int)> get_client_fd);

    using AccessLogCallback = std::function<void(int client_fd, const RequestInfo&)>;
    void set_access_log_callback(AccessLogCallback callback);

    void set_custom_header_modifier(std::function<void(lb::http::HttpRequest&)> modifier);

private:
    net::EpollReactor& reactor_;
    std::function<void(int)> start_backpressure_;
    std::function<void(int)> clear_backpressure_;
    std::function<void(int)> close_connection_;
    std::function<std::string(int)> get_client_ip_;
    std::function<int(int)> get_client_fd_;
    AccessLogCallback access_log_callback_;
    std::function<void(lb::http::HttpRequest&)> custom_header_modifier_;
    bool is_https_;

    struct HttpState {
        lb::http::HttpRequestParser parser;
        bool request_parsed_;
        std::vector<uint8_t> pending_data_;
        RequestInfo request_info;
    };
    std::unordered_map<int, HttpState> http_states_;
    std::unordered_map<int, int> backend_to_client_map_;

    bool parse_and_modify_http_request(net::Connection* conn, int fd);
    static std::vector<uint8_t> serialize_http_request(const lb::http::HttpRequest& request);
};

} // namespace lb::core
