#include "core/backend_node.h"

namespace lb::core {

BackendNode::BackendNode(const std::string& host, uint16_t port)
    : host_(host), port_(port),
      state_(BackendState::HEALTHY),
      active_connections_(0),
      failure_count_(0) {
}

} // namespace lb::core

