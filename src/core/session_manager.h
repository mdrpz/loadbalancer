#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace lb::core {

struct SessionInfo {
    std::string backend_host;
    uint16_t backend_port;
    std::chrono::steady_clock::time_point expires_at;
};

class SessionManager {
public:
    SessionManager();
    ~SessionManager() = default;

    [[nodiscard]] std::optional<SessionInfo> get_session(const std::string& key) const;
    void set_session(const std::string& key, const std::string& host, uint16_t port,
                     uint32_t ttl_seconds);
    void clear_session(const std::string& key);
    void cleanup_expired_sessions();
    void clear_all_sessions();

    [[nodiscard]] size_t size() const {
        return sessions_.size();
    }

private:
    std::unordered_map<std::string, SessionInfo> sessions_;
    mutable std::mutex mutex_;
};

} // namespace lb::core
