#pragma once

#include "net/connection.h"
#include <memory>
#include <functional>

namespace lb::core {

class RetryHandler {
public:
    RetryHandler(std::function<void(std::unique_ptr<net::Connection>, int)> connect_callback);
    
    void retry(std::unique_ptr<net::Connection> client_conn, int retry_count);

private:
    static constexpr int MAX_RETRY_ATTEMPTS = 3;
    std::function<void(std::unique_ptr<net::Connection>, int)> connect_callback_;
};

} // namespace lb::core

