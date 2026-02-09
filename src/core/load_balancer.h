#pragma once

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>
#include "core/backend_connector.h"
#include "core/backend_node.h"
#include "core/backend_pool.h"
#include "core/backpressure_manager.h"
#include "core/connection_manager.h"
#include "core/connection_pool.h"
#include "core/data_forwarder.h"
#include "core/event_handlers.h"
#include "core/http_data_forwarder.h"
#include "core/memory_budget.h"
#include "core/retry_handler.h"
#include "core/session_manager.h"
#include "core/splice_forwarder.h"
#include "core/thread_pool.h"
#include "health/health_checker.h"
#include "net/connection.h"
#include "net/epoll_reactor.h"
#include "net/tcp_listener.h"
#include "tls/tls_context.h"

namespace lb::config {
struct Config;
class ConfigManager;
} // namespace lb::config

namespace lb::core {

class LoadBalancer {
public:
    LoadBalancer();
    ~LoadBalancer();

    bool initialize(const std::string& listen_host, uint16_t listen_port);
    bool initialize_from_config(const std::shared_ptr<const lb::config::Config>& config);
    void run();
    void stop();
    void shutdown_gracefully();

    void set_config_manager(lb::config::ConfigManager* config_manager);

    void add_backend(const std::string& host, uint16_t port, uint32_t weight = 1);

    void apply_config(const std::shared_ptr<const lb::config::Config>& config);

    ThreadPool* thread_pool() const;

private:
    void handle_accept();

    void cleanup_drained_backends();
    void check_request_timeouts();
    bool is_ip_allowed(const std::string& client_ip,
                       const std::shared_ptr<const lb::config::Config>& config) const;
    void send_http_error_and_close(int client_fd, const lb::http::HttpResponse& response);
    bool check_rate_limit(const std::string& client_ip,
                          const std::shared_ptr<const lb::config::Config>& config);
    void cleanup_rate_limit_entries();
    void try_dequeue_clients();
    std::string get_session_key(int client_fd, const std::string& client_ip,
                                const std::shared_ptr<const lb::config::Config>& config) const;
    void attach_memory_accounting(net::Connection* conn);
    void check_graceful_shutdown();
    void force_close_all_connections();

    std::unique_ptr<net::EpollReactor> reactor_;
    std::unique_ptr<net::TcpListener> listener_;
    std::unique_ptr<BackendPool> backend_pool_;
    std::unique_ptr<lb::health::HealthChecker> health_checker_;
    std::unique_ptr<lb::tls::TlsContext> tls_context_;

    std::unordered_map<int, std::unique_ptr<net::Connection>> connections_;

    std::unordered_map<int, std::weak_ptr<BackendNode>> backend_connections_;

    std::unordered_map<int, std::chrono::steady_clock::time_point> connection_times_;
    std::unordered_map<int, std::chrono::steady_clock::time_point> handshake_start_times_;

    std::unordered_map<int, int> backend_to_client_map_;

    std::unordered_map<int, int> client_retry_counts_;

    uint32_t max_global_connections_;
    uint32_t max_connections_per_backend_;

    std::unordered_map<int, std::chrono::steady_clock::time_point> backpressure_start_times_;
    uint32_t backpressure_timeout_ms_;

    static constexpr int MAX_RETRY_ATTEMPTS = 3;
    uint32_t connection_timeout_seconds_;
    uint32_t request_timeout_ms_;

    std::unique_ptr<ConnectionManager> connection_manager_;
    std::unique_ptr<ConnectionPoolManager> pool_manager_;
    std::unique_ptr<EventHandlers> event_handlers_;
    std::unique_ptr<BackendConnector> backend_connector_;
    std::unique_ptr<DataForwarder> data_forwarder_;
    std::unique_ptr<SpliceForwarder> splice_forwarder_;
    std::unique_ptr<HttpDataForwarder> http_data_forwarder_;
    bool use_splice_ = false;
    bool pool_enabled_ = true;
    std::unique_ptr<BackpressureManager> backpressure_manager_;
    std::unique_ptr<RetryHandler> retry_handler_;

    lb::config::ConfigManager* config_manager_;

    std::string mode_;

    std::shared_ptr<const lb::config::Config> last_applied_config_;

    std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>>
        rate_limit_tracker_;
    std::mutex rate_limit_mutex_;

    struct QueuedClient {
        int fd;
        std::string client_ip;
        std::chrono::steady_clock::time_point enqueue_time;
    };
    std::deque<QueuedClient> pending_clients_;

    std::unique_ptr<SessionManager> session_manager_;
    std::unordered_map<int, std::string> client_session_keys_;

    std::shared_ptr<MemoryBudget> memory_budget_;

    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> draining_{false};
    std::chrono::steady_clock::time_point shutdown_deadline_;
    uint32_t graceful_shutdown_timeout_seconds_{30};
    uint32_t tls_handshake_timeout_ms_{10000};

    void check_handshake_timeouts();
    void check_backpressure_timeouts();

    std::unique_ptr<ThreadPool> thread_pool_;

    std::shared_ptr<const lb::config::Config> pending_config_;
    std::mutex pending_config_mutex_;
    std::atomic<bool> config_check_in_progress_{false};
};

} // namespace lb::core
