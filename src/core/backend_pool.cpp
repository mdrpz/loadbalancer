#include "core/backend_pool.h"
#include <algorithm>

namespace lb::core {

BackendPool::BackendPool(RoutingAlgorithm algorithm)
    : algorithm_(algorithm), round_robin_index_(0) {
}

void BackendPool::add_backend(std::shared_ptr<BackendNode> backend) {
    backends_.push_back(backend);
}

void BackendPool::remove_backend(const std::string& host, uint16_t port) {
    backends_.erase(
        std::remove_if(backends_.begin(), backends_.end(),
            [&](const std::shared_ptr<BackendNode>& node) {
                return node->host() == host && node->port() == port;
            }),
        backends_.end()
    );
}

std::shared_ptr<BackendNode> BackendPool::select_backend(uint32_t max_connections_per_backend) {
    if (backends_.empty()) {
        return nullptr;
    }

    // Filter to only healthy backends that are under their connection limit
    std::vector<std::shared_ptr<BackendNode>> healthy;
    for (const auto& backend : backends_) {
        if (backend->state() == BackendState::HEALTHY) {
            // Check connection limit (0 means no limit)
            if (max_connections_per_backend == 0 || 
                backend->active_connections() < max_connections_per_backend) {
                healthy.push_back(backend);
            }
        }
    }

    if (healthy.empty()) {
        return nullptr; // No healthy backends available (all at limit or unhealthy)
    }

    if (algorithm_ == RoutingAlgorithm::ROUND_ROBIN) {
        auto selected = healthy[round_robin_index_ % healthy.size()];
        round_robin_index_++;
        return selected;
    } else if (algorithm_ == RoutingAlgorithm::LEAST_CONNECTIONS) {
        auto min_it = std::min_element(healthy.begin(), healthy.end(),
            [](const std::shared_ptr<BackendNode>& a,
               const std::shared_ptr<BackendNode>& b) {
                return a->active_connections() < b->active_connections();
            });
        return *min_it;
    }

    return nullptr;
}

std::shared_ptr<BackendNode> BackendPool::find_backend(const std::string& host, uint16_t port) const {
    for (const auto& backend : backends_) {
        if (backend->host() == host && backend->port() == port) {
            return backend;
        }
    }
    return nullptr;
}

} // namespace lb::core

