#pragma once

#include "core/backend_node.h"
#include <vector>
#include <memory>
#include <cstdint>

namespace lb::core {

enum class RoutingAlgorithm {
    ROUND_ROBIN,
    LEAST_CONNECTIONS
};

class BackendPool {
public:
    BackendPool(RoutingAlgorithm algorithm = RoutingAlgorithm::ROUND_ROBIN);
    
    void add_backend(std::shared_ptr<BackendNode> backend);
    void remove_backend(const std::string& host, uint16_t port);
    
    // Select next backend for routing
    std::shared_ptr<BackendNode> select_backend();
    
    void set_algorithm(RoutingAlgorithm algorithm) { algorithm_ = algorithm; }
    RoutingAlgorithm algorithm() const { return algorithm_; }
    
    size_t size() const { return backends_.size(); }

private:
    std::vector<std::shared_ptr<BackendNode>> backends_;
    RoutingAlgorithm algorithm_;
    uint32_t round_robin_index_; // For ROUND_ROBIN algorithm
};

} // namespace lb::core

