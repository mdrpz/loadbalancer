#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "core/backend_node.h"

namespace lb::core {

enum class RoutingAlgorithm { ROUND_ROBIN, LEAST_CONNECTIONS };

class BackendPool {
public:
    BackendPool(RoutingAlgorithm algorithm = RoutingAlgorithm::ROUND_ROBIN);

    void add_backend(const std::shared_ptr<BackendNode>& backend);
    void remove_backend(const std::string& host, uint16_t port);

    // Select next backend for routing
    // max_connections_per_backend: 0 means no limit
    std::shared_ptr<BackendNode> select_backend(uint32_t max_connections_per_backend = 0);

    void set_algorithm(RoutingAlgorithm algorithm) {
        algorithm_ = algorithm;
    }
    [[nodiscard]] RoutingAlgorithm algorithm() const {
        return algorithm_;
    }

    [[nodiscard]] size_t size() const {
        return backends_.size();
    }

    // Get all backends (for config reload tracking)
    [[nodiscard]] std::vector<std::shared_ptr<BackendNode>> get_all_backends() const {
        return backends_;
    }

    // Find backend by host:port
    [[nodiscard]] std::shared_ptr<BackendNode> find_backend(const std::string& host,
                                                            uint16_t port) const;

private:
    std::vector<std::shared_ptr<BackendNode>> backends_;
    RoutingAlgorithm algorithm_;
    uint32_t round_robin_index_; // For ROUND_ROBIN algorithm
};

} // namespace lb::core
