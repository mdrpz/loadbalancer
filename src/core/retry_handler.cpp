#include "core/retry_handler.h"

#include <utility>

#include <utility>
#include "metrics/metrics.h"

namespace lb::core {

RetryHandler::RetryHandler(
    std::function<void(std::unique_ptr<net::Connection>, int)> connect_callback)
    : connect_callback_(std::move(std::move(connect_callback))) {}

void RetryHandler::retry(std::unique_ptr<net::Connection> client_conn, int retry_count) {
    if (retry_count >= MAX_RETRY_ATTEMPTS) {
        lb::metrics::Metrics::instance().increment_backend_routes_failed();
        client_conn->close();
        return;
    }

    retry_count++;
    connect_callback_(std::move(client_conn), retry_count);
}

} // namespace lb::core
