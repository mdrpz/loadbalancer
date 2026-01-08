#include "core/event_handlers.h"
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <sstream>
#include <utility>
#include "logging/logger.h"
#include "metrics/metrics.h"

namespace lb::core {

EventHandlers::EventHandlers(
    std::unordered_map<int, std::unique_ptr<net::Connection>>& connections,
    std::unordered_map<int, std::weak_ptr<BackendNode>>& backend_connections,
    std::unordered_map<int, std::chrono::steady_clock::time_point>& connection_times,
    std::unordered_map<int, int>& backend_to_client_map,
    std::unordered_map<int, int>& client_retry_counts, net::EpollReactor& reactor,
    uint32_t connection_timeout_seconds, std::function<net::Connection*(int)> get_connection,
    std::function<void(int)> check_backpressure, std::function<void(int)> close_connection,
    std::function<void(int)> close_backend_only,
    std::function<void(net::Connection*, net::Connection*)> forward_data,
    std::function<void(int)> clear_backpressure,
    std::function<void(std::unique_ptr<net::Connection>, int)> retry,
    std::function<void(int)> on_tls_handshake_complete, bool use_splice)
    : connections_(connections), backend_connections_(backend_connections),
      connection_times_(connection_times), backend_to_client_map_(backend_to_client_map),
      client_retry_counts_(client_retry_counts), reactor_(reactor),
      connection_timeout_seconds_(connection_timeout_seconds),
      get_connection_(std::move(std::move(get_connection))),
      check_backpressure_(std::move(std::move(check_backpressure))),
      close_connection_(std::move(std::move(close_connection))),
      close_backend_only_(std::move(std::move(close_backend_only))),
      forward_data_(std::move(std::move(forward_data))),
      clear_backpressure_(std::move(std::move(clear_backpressure))),
      retry_(std::move(std::move(retry))),
      on_tls_handshake_complete_(std::move(on_tls_handshake_complete)), use_splice_(use_splice) {}

bool EventHandlers::try_retry_on_backend_error(int backend_fd) {
    auto client_it = backend_to_client_map_.find(backend_fd);
    if (client_it == backend_to_client_map_.end())
        return false;

    int client_fd = client_it->second;
    auto client_conn_it = connections_.find(client_fd);
    if (client_conn_it == connections_.end())
        return false;

    int retry_count = 0;
    auto retry_it = client_retry_counts_.find(client_fd);
    if (retry_it != client_retry_counts_.end())
        retry_count = retry_it->second;

    close_backend_only_(backend_fd);
    auto client_conn = std::move(client_conn_it->second);
    connections_.erase(client_conn_it);
    retry_(std::move(client_conn), retry_count);
    return true;
}

void EventHandlers::handle_client_event(int fd, net::EventType type) {
    auto* conn = get_connection_(fd);
    if (!conn)
        return;

    if (conn->state() == net::ConnectionState::CLOSED)
        return;

    check_backpressure_(fd);

    if (type == net::EventType::ERROR || type == net::EventType::HUP) {
        close_connection_(fd);
        return;
    }

    if (conn->state() == net::ConnectionState::HANDSHAKE) {
        if (type == net::EventType::READ || type == net::EventType::WRITE) {
            if (!handle_tls_handshake(conn, fd)) {
                close_connection_(fd);
            }
        }
        return;
    }

    if (type == net::EventType::READ) {
        if (conn->state() != net::ConnectionState::ESTABLISHED)
            return;

        if (use_splice_ && conn->peer() &&
            conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
            forward_data_(conn, conn->peer());
            return;
        }

        bool read_success = conn->read_from_fd();
        if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
            if (!conn->read_buffer().empty()) {
                forward_data_(conn, conn->peer());
            }
        }

        if (!read_success) {
            if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
                if (!conn->read_buffer().empty())
                    forward_data_(conn, conn->peer());
            }

            if (conn->fd() >= 0)
                ::shutdown(conn->fd(), SHUT_WR);
            if (conn->peer() && conn->peer()->fd() >= 0)
                ::shutdown(conn->peer()->fd(), SHUT_WR);
            return;
        }
    }

    if (type == net::EventType::WRITE) {
        if (conn->state() != net::ConnectionState::ESTABLISHED)
            return;

        if (!conn->write_to_fd())
            close_connection_(fd);

        if (conn->write_buffer().empty())
            clear_backpressure_(fd);
        if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED)
            reactor_.mod_fd(conn->peer()->fd(), EPOLLIN | EPOLLOUT);
        if (conn->peer())
            clear_backpressure_(conn->peer()->fd());
    }
}

void EventHandlers::handle_backend_event(int fd, net::EventType type) {
    auto* conn = get_connection_(fd);
    if (!conn)
        return;

    if (conn->state() == net::ConnectionState::CLOSED)
        return;

    check_backpressure_(fd);

    if (type == net::EventType::ERROR || type == net::EventType::HUP) {
        if (conn->state() == net::ConnectionState::CONNECTING) {
            auto backend_it = backend_connections_.find(fd);
            if (backend_it != backend_connections_.end()) {
                if (auto backend_node = backend_it->second.lock()) {
                    backend_node->increment_failures();
                    std::ostringstream backend_str;
                    backend_str << backend_node->host() << ":" << backend_node->port();
                    lb::metrics::Metrics::instance().increment_backend_failures(backend_str.str());
                }
            }

            if (try_retry_on_backend_error(fd)) {
                return;
            }
        }

        close_connection_(fd);
        return;
    }

    if (conn->state() == net::ConnectionState::CONNECTING) {
        auto it_time = connection_times_.find(fd);
        if (it_time != connection_times_.end()) {
            auto elapsed = std::chrono::steady_clock::now() - it_time->second;
            if (elapsed > std::chrono::seconds(connection_timeout_seconds_)) {
                auto backend_it = backend_connections_.find(fd);
                if (backend_it != backend_connections_.end()) {
                    if (auto backend_node = backend_it->second.lock()) {
                        backend_node->increment_failures();
                        std::ostringstream backend_str;
                        backend_str << backend_node->host() << ":" << backend_node->port();
                        lb::metrics::Metrics::instance().increment_backend_failures(
                            backend_str.str());
                    }
                }

                if (try_retry_on_backend_error(fd)) {
                    return;
                }

                close_connection_(fd);
                return;
            }
        }

        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
            conn->set_state(net::ConnectionState::ESTABLISHED);
            auto backend_it = backend_connections_.find(fd);
            if (backend_it != backend_connections_.end()) {
                if (auto backend_node = backend_it->second.lock()) {
                    backend_node->increment_connections();
                    std::ostringstream backend_str;
                    backend_str << backend_node->host() << ":" << backend_node->port();
                    lb::metrics::Metrics::instance().increment_backend_routed(backend_str.str());
                    lb::metrics::Metrics::instance().increment_connections_active();
                }
            }

            reactor_.mod_fd(fd, EPOLLIN | EPOLLOUT);
            if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED &&
                !conn->peer()->read_buffer().empty())
                forward_data_(conn->peer(), conn);
        } else {
            auto backend_it = backend_connections_.find(fd);
            if (backend_it != backend_connections_.end()) {
                if (auto backend_node = backend_it->second.lock()) {
                    backend_node->increment_failures();
                    std::ostringstream backend_str;
                    backend_str << backend_node->host() << ":" << backend_node->port();
                    lb::metrics::Metrics::instance().increment_backend_failures(backend_str.str());
                }
            }

            if (try_retry_on_backend_error(fd)) {
                return;
            }

            close_connection_(fd);
            return;
        }
    }

    if (type == net::EventType::READ) {
        if (conn->state() != net::ConnectionState::ESTABLISHED)
            return;

        if (use_splice_ && conn->peer() &&
            conn->peer()->state() == net::ConnectionState::ESTABLISHED) {
            forward_data_(conn, conn->peer());
            return;
        }

        bool read_success = conn->read_from_fd();

        if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED &&
            !conn->read_buffer().empty())
            forward_data_(conn, conn->peer());

        if (!read_success) {
            close_connection_(fd);
            return;
        }
    }

    if (type == net::EventType::WRITE) {
        if (conn->state() != net::ConnectionState::ESTABLISHED)
            return;

        if (!conn->write_to_fd()) {
            close_connection_(fd);
            return;
        }

        if (conn->write_buffer().empty())
            clear_backpressure_(fd);
        if (conn->peer() && conn->peer()->state() == net::ConnectionState::ESTABLISHED)
            reactor_.mod_fd(conn->peer()->fd(), EPOLLIN | EPOLLOUT);
        if (conn->peer())
            clear_backpressure_(conn->peer()->fd());
    }
}

bool EventHandlers::handle_tls_handshake(net::Connection* conn, int fd) {
    if (!conn || !conn->is_tls()) {
        return false;
    }

    SSL* ssl = conn->ssl();
    if (!ssl) {
        lb::logging::Logger::instance().error("TLS: SSL object is null for fd=" +
                                              std::to_string(fd));
        return false;
    }

    int result = 0;
    try {
        result = SSL_accept(ssl);
    } catch (...) {
        lb::logging::Logger::instance().error("TLS: Exception during SSL_accept for fd=" +
                                              std::to_string(fd));
        return false;
    }

    if (result == 1) {
        conn->set_state(net::ConnectionState::ESTABLISHED);
        lb::logging::Logger::instance().debug("TLS handshake completed (fd=" + std::to_string(fd) +
                                              ")");

        reactor_.mod_fd(fd, EPOLLIN | EPOLLOUT);

        if (on_tls_handshake_complete_) {
            on_tls_handshake_complete_(fd);
        }
        return true;
    }

    int ssl_error = SSL_ERROR_NONE;
    try {
        ssl_error = SSL_get_error(ssl, result);
    } catch (...) {
        lb::logging::Logger::instance().error("TLS: Exception during SSL_get_error for fd=" +
                                              std::to_string(fd));
        return false;
    }

    if (ssl_error == SSL_ERROR_WANT_READ) {
        reactor_.mod_fd(fd, EPOLLIN);
        return true;
    }

    if (ssl_error == SSL_ERROR_WANT_WRITE) {
        reactor_.mod_fd(fd, EPOLLOUT);
        return true;
    }

    unsigned long err = ERR_get_error();
    std::string error_msg = "TLS handshake failed (fd=" + std::to_string(fd) +
                            ", SSL_get_error: " + std::to_string(ssl_error) + ")";
    if (err != 0) {
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        error_msg += ": " + std::string(err_buf);
    }
    lb::logging::Logger::instance().error(error_msg);
    return false;
}

} // namespace lb::core
