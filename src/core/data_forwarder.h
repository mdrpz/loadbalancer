#pragma once

#include "net/connection.h"
#include "net/epoll_reactor.h"
#include <functional>

namespace lb::core {

class DataForwarder {
public:
    DataForwarder(net::EpollReactor& reactor,
                  std::function<void(int)> start_backpressure,
                  std::function<void(int)> clear_backpressure,
                  std::function<void(int)> close_connection);
    
    void forward(net::Connection* from, net::Connection* to);

private:
    net::EpollReactor& reactor_;
    std::function<void(int)> start_backpressure_;
    std::function<void(int)> clear_backpressure_;
    std::function<void(int)> close_connection_;
};

} // namespace lb::core

