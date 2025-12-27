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

std::shared_ptr<BackendNode> BackendPool::select_backend() {
    if (backends_.empty()) {
        return nullptr;
    }

    // Filter to only healthy backends
    std::vector<std::shared_ptr<BackendNode>> healthy;
    for (const auto& backend : backends_) {
        if (backend->state() == BackendState::HEALTHY) {
            healthy.push_back(backend);
        }
    }

    if (healthy.empty()) {
        return nullptr; // No healthy backends
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

} // namespace lb::core

