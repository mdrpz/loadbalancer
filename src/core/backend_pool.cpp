#include "core/backend_pool.h"
#include <algorithm>

namespace lb::core {

BackendPool::BackendPool(RoutingAlgorithm algorithm)
    : algorithm_(algorithm), round_robin_index_(0), weighted_counter_(0) {}

void BackendPool::add_backend(const std::shared_ptr<BackendNode>& backend) {
    backends_.push_back(backend);
}

void BackendPool::remove_backend(const std::string& host, uint16_t port) {
    backends_.erase(std::remove_if(backends_.begin(), backends_.end(),
                                   [&](const std::shared_ptr<BackendNode>& node) {
                                       return node->host() == host && node->port() == port;
                                   }),
                    backends_.end());
}

std::shared_ptr<BackendNode> BackendPool::select_backend(uint32_t max_connections_per_backend) {
    if (backends_.empty())
        return nullptr;

    std::vector<std::shared_ptr<BackendNode>> healthy;
    for (const auto& backend : backends_) {
        if (backend->state() == BackendState::HEALTHY) {
            if (max_connections_per_backend == 0 ||
                backend->active_connections() < max_connections_per_backend) {
                healthy.push_back(backend);
            }
        }
    }

    if (healthy.empty())
        return nullptr;

    if (algorithm_ == RoutingAlgorithm::ROUND_ROBIN) {
        return select_weighted_round_robin(healthy);
    }
    if (algorithm_ == RoutingAlgorithm::LEAST_CONNECTIONS) {
        auto min_it = std::min_element(
            healthy.begin(), healthy.end(),
            [](const std::shared_ptr<BackendNode>& a, const std::shared_ptr<BackendNode>& b) {
                double load_a = static_cast<double>(a->active_connections()) / a->weight();
                double load_b = static_cast<double>(b->active_connections()) / b->weight();
                return load_a < load_b;
            });
        return *min_it;
    }

    return nullptr;
}

std::shared_ptr<BackendNode> BackendPool::select_weighted_round_robin(
    const std::vector<std::shared_ptr<BackendNode>>& healthy) {
    uint32_t total_weight = 0;
    for (const auto& backend : healthy) {
        total_weight += backend->weight();
    }

    if (total_weight == 0)
        return healthy[0];

    uint32_t target = weighted_counter_ % total_weight;
    weighted_counter_++;

    uint32_t cumulative = 0;
    for (const auto& backend : healthy) {
        cumulative += backend->weight();
        if (target < cumulative) {
            return backend;
        }
    }

    return healthy[0];
}

std::shared_ptr<BackendNode> BackendPool::find_backend(const std::string& host,
                                                       uint16_t port) const {
    for (const auto& backend : backends_) {
        if (backend->host() == host && backend->port() == port)
            return backend;
    }
    return nullptr;
}

} // namespace lb::core
