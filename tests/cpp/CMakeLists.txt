cmake_minimum_required(VERSION 3.20)

add_executable(test_routing test_routing.cpp)
target_link_libraries(test_routing PRIVATE lb_lib)
add_test(NAME routing_tests COMMAND test_routing)

add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE lb_lib)
add_test(NAME config_tests COMMAND test_config)

add_executable(test_health test_health.cpp)
target_link_libraries(test_health PRIVATE lb_lib)
add_test(NAME health_tests COMMAND test_health)

add_executable(test_memory_budget test_memory_budget.cpp)
target_link_libraries(test_memory_budget PRIVATE lb_lib)
add_test(NAME memory_budget_tests COMMAND test_memory_budget)
