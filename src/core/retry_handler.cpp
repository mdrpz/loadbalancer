#include "core/retry_handler.h"
#include "metrics/metrics.h"

namespace lb::core {

RetryHandler::RetryHandler(std::function<void(std::unique_ptr<net::Connection>, int)> connect_callback)
    : connect_callback_(connect_callback) {
}

void RetryHandler::retry(std::unique_ptr<net::Connection> client_conn, int retry_count) {
    // Check retry limit
    if (retry_count >= MAX_RETRY_ATTEMPTS) {
        // Max retries exceeded - reject client
        lb::metrics::Metrics::instance().increment_backend_routes_failed();
        client_conn->close();
        return;
    }
    
    // Increment retry count
    retry_count++;
    
    // Try connecting to next backend
    connect_callback_(std::move(client_conn), retry_count);
}

} // namespace lb::core

