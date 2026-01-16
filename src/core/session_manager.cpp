#include "core/session_manager.h"
#include <mutex>

namespace lb::core {

SessionManager::SessionManager() = default;

std::optional<SessionInfo> SessionManager::get_session(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(key);
    if (it == sessions_.end()) {
        return std::nullopt;
    }

    const auto& session = it->second;
    auto now = std::chrono::steady_clock::now();
    if (now >= session.expires_at) {
        return std::nullopt;
    }

    return session;
}

void SessionManager::set_session(const std::string& key, const std::string& host, uint16_t port,
                                 uint32_t ttl_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    auto expires_at = now + std::chrono::seconds(ttl_seconds);

    SessionInfo session;
    session.backend_host = host;
    session.backend_port = port;
    session.expires_at = expires_at;

    sessions_[key] = session;
}

void SessionManager::clear_session(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(key);
}

void SessionManager::cleanup_expired_sessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    auto it = sessions_.begin();
    while (it != sessions_.end()) {
        if (now >= it->second.expires_at) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void SessionManager::clear_all_sessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
}

} // namespace lb::core
