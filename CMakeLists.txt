cmake_minimum_required(VERSION 3.20)
project(loadbalancer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCH "Build benchmarks" OFF)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Find yaml-cpp for config parsing (Phase 3)
find_package(yaml-cpp QUIET)
if(yaml-cpp_FOUND)
    message(STATUS "Found yaml-cpp: ${yaml-cpp_VERSION}")
else()
    message(STATUS "yaml-cpp not found - will be done in phase 3")
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# Create library with all source files except main.cpp
add_library(lb_lib STATIC
    src/net/tcp_listener.cpp
    src/net/epoll_reactor.cpp
    src/net/connection.cpp
    src/core/backend_pool.cpp
    src/core/backend_node.cpp
    src/core/load_balancer.cpp
    src/core/connection_manager.cpp
    src/core/event_handlers.cpp
    src/core/backend_connector.cpp
    src/core/data_forwarder.cpp
    src/core/backpressure_manager.cpp
    src/core/retry_handler.cpp
    src/config/config.cpp
    src/health/health_checker.cpp
    src/metrics/metrics.cpp
    src/metrics/metrics_server.cpp
    src/tls/tls_context.cpp
    src/logging/logger.cpp
)

# Link libraries to the library target
target_link_libraries(lb_lib
    PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

# Conditionally link yaml-cpp
if(yaml-cpp_FOUND)
    target_link_libraries(lb_lib PRIVATE yaml-cpp)
    target_compile_definitions(lb_lib PRIVATE HAVE_YAML_CPP)
endif()

# Create executable that links to the library
add_executable(lb src/main.cpp)
target_link_libraries(lb PRIVATE lb_lib)

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/cpp)
endif()

# Benchmarks
if(BUILD_BENCH)
    add_subdirectory(bench)
endif()

