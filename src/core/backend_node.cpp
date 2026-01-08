#include "core/backend_node.h"

#include <utility>

namespace lb::core {

BackendNode::BackendNode(std::string host, uint16_t port, uint32_t weight)
    : host_(std::move(host)), port_(port), weight_(weight > 0 ? weight : 1),
      state_(BackendState::HEALTHY), active_connections_(0), failure_count_(0) {}

} // namespace lb::core
