cmake_minimum_required(VERSION 3.20)

add_executable(test_routing test_routing.cpp)
target_link_libraries(test_routing PRIVATE lb_lib)

add_test(NAME routing_tests COMMAND test_routing)

