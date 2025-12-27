cmake_minimum_required(VERSION 3.20)

# Find testing framework (Catch2 or GoogleTest)
find_package(Catch2 QUIET)
if(Catch2_FOUND)
    message(STATUS "Using Catch2 for testing")
    include(Catch)
    
    add_executable(test_routing
        test_routing.cpp
    )
    target_link_libraries(test_routing PRIVATE lb_lib Catch2::Catch2)
    catch_discover_tests(test_routing)
else()
    message(STATUS "Catch2 not found - tests will be built but may need manual configuration")
    add_executable(test_routing
        test_routing.cpp
    )
    target_link_libraries(test_routing PRIVATE lb_lib)
endif()

