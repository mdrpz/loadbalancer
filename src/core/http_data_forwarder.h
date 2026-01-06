#pragma once

#include <functional>
#include <unordered_map>
#include "http/http_handler.h"
#include "http/http_request.h"
#include "net/connection.h"
#include "net/epoll_reactor.h"

namespace lb::core {

class HttpDataForwarder {
public:
    HttpDataForwarder(net::EpollReactor& reactor, std::function<void(int)> start_backpressure,
                      std::function<void(int)> clear_backpressure,
                      std::function<void(int)> close_connection,
                      std::function<std::string(int)> get_client_ip, bool is_https);

    void forward(net::Connection* from, net::Connection* to);

private:
    net::EpollReactor& reactor_;
    std::function<void(int)> start_backpressure_;
    std::function<void(int)> clear_backpressure_;
    std::function<void(int)> close_connection_;
    std::function<std::string(int)> get_client_ip_;
    bool is_https_;

    struct HttpState {
        lb::http::HttpRequestParser parser;
        bool request_parsed_;
        std::vector<uint8_t> pending_data_;
    };
    std::unordered_map<int, HttpState> http_states_;

    bool parse_and_modify_http_request(net::Connection* conn, int fd);
    static std::vector<uint8_t> serialize_http_request(const lb::http::HttpRequest& request);
};

} // namespace lb::core
