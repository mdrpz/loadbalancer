#include "core/backend_node.h"

#include <utility>

namespace lb::core {

BackendNode::BackendNode(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port), state_(BackendState::HEALTHY), active_connections_(0),
      failure_count_(0) {}

} // namespace lb::core
