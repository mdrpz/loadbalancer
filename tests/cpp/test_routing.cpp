#include <cassert>
#include <iostream>
#include "core/backend_node.h"
#include "core/backend_pool.h"

void test_round_robin() {
    std::cout << "Testing round-robin routing...\n";

    lb::core::BackendPool pool(lb::core::RoutingAlgorithm::ROUND_ROBIN);

    auto backend1 = std::make_shared<lb::core::BackendNode>("10.0.0.1", 8000);
    auto backend2 = std::make_shared<lb::core::BackendNode>("10.0.0.2", 8000);
    auto backend3 = std::make_shared<lb::core::BackendNode>("10.0.0.3", 8000);

    pool.add_backend(backend1);
    pool.add_backend(backend2);
    pool.add_backend(backend3);

    // Test round-robin distribution
    auto selected1 = pool.select_backend();
    assert(selected1 != nullptr);
    assert(selected1->host() == "10.0.0.1" || selected1->host() == "10.0.0.2" ||
           selected1->host() == "10.0.0.3");

    auto selected2 = pool.select_backend();
    assert(selected2 != nullptr);

    auto selected3 = pool.select_backend();
    assert(selected3 != nullptr);

    // After 3 selections, should cycle back
    auto selected4 = pool.select_backend();
    assert(selected4 != nullptr);

    std::cout << "Round-robin test passed!\n";
}

void test_least_connections() {
    std::cout << "Testing least-connections routing...\n";

    lb::core::BackendPool pool(lb::core::RoutingAlgorithm::LEAST_CONNECTIONS);

    auto backend1 = std::make_shared<lb::core::BackendNode>("10.0.0.1", 8000);
    auto backend2 = std::make_shared<lb::core::BackendNode>("10.0.0.2", 8000);
    auto backend3 = std::make_shared<lb::core::BackendNode>("10.0.0.3", 8000);

    pool.add_backend(backend1);
    pool.add_backend(backend2);
    pool.add_backend(backend3);

    // All should have 0 connections initially
    auto selected1 = pool.select_backend();
    assert(selected1 != nullptr);
    assert(selected1->active_connections() == 0);

    // Increment connections on backend1
    backend1->increment_connections();
    backend1->increment_connections();
    backend2->increment_connections();

    // Should select backend3 (least connections)
    auto selected2 = pool.select_backend();
    assert(selected2 != nullptr);
    assert(selected2->active_connections() == 0);
    assert(selected2->host() == "10.0.0.3");

    std::cout << "Least-connections test passed!\n";
}

void test_unhealthy_backend_filtering() {
    std::cout << "Testing unhealthy backend filtering...\n";

    lb::core::BackendPool pool(lb::core::RoutingAlgorithm::ROUND_ROBIN);

    auto backend1 = std::make_shared<lb::core::BackendNode>("10.0.0.1", 8000);
    auto backend2 = std::make_shared<lb::core::BackendNode>("10.0.0.2", 8000);
    auto backend3 = std::make_shared<lb::core::BackendNode>("10.0.0.3", 8000);

    pool.add_backend(backend1);
    pool.add_backend(backend2);
    pool.add_backend(backend3);

    // Mark backend1 and backend2 as unhealthy
    backend1->set_state(lb::core::BackendState::UNHEALTHY);
    backend2->set_state(lb::core::BackendState::UNHEALTHY);

    // Should only select backend3
    auto selected = pool.select_backend();
    assert(selected != nullptr);
    assert(selected->host() == "10.0.0.3");

    std::cout << "Unhealthy backend filtering test passed!\n";
}

void test_empty_pool() {
    std::cout << "Testing empty pool handling...\n";

    lb::core::BackendPool pool(lb::core::RoutingAlgorithm::ROUND_ROBIN);

    // Should return nullptr when no backends
    auto selected = pool.select_backend();
    assert(selected == nullptr);

    std::cout << "Empty pool test passed!\n";
}

void test_weighted_round_robin() {
    std::cout << "Testing weighted round-robin routing...\n";

    lb::core::BackendPool pool(lb::core::RoutingAlgorithm::ROUND_ROBIN);

    // Backend1 has weight 3, Backend2 has weight 1
    auto backend1 = std::make_shared<lb::core::BackendNode>("10.0.0.1", 8000, 3);
    auto backend2 = std::make_shared<lb::core::BackendNode>("10.0.0.2", 8000, 1);

    pool.add_backend(backend1);
    pool.add_backend(backend2);

    // Count selections over 20 requests
    int count1 = 0, count2 = 0;
    for (int i = 0; i < 20; i++) {
        auto selected = pool.select_backend();
        assert(selected != nullptr);
        if (selected->host() == "10.0.0.1") count1++;
        else if (selected->host() == "10.0.0.2") count2++;
    }

    // With weights 3:1, expect roughly 15:5 distribution
    // Allow some tolerance, but backend1 should get more
    assert(count1 > count2);  // Backend1 (weight 3) should get more traffic
    assert(count1 == 15);     // Exactly 15 for weight 3 out of 20
    assert(count2 == 5);      // Exactly 5 for weight 1 out of 20

    std::cout << "Weighted round-robin test passed! (backend1: " << count1 
              << ", backend2: " << count2 << ")\n";
}

void test_weighted_least_connections() {
    std::cout << "Testing weighted least-connections routing...\n";

    lb::core::BackendPool pool(lb::core::RoutingAlgorithm::LEAST_CONNECTIONS);

    // Backend1 has weight 2, Backend2 has weight 1
    auto backend1 = std::make_shared<lb::core::BackendNode>("10.0.0.1", 8000, 2);
    auto backend2 = std::make_shared<lb::core::BackendNode>("10.0.0.2", 8000, 1);

    pool.add_backend(backend1);
    pool.add_backend(backend2);

    // Both have 0 connections, backend1 should be preferred (lower load/weight)
    auto selected1 = pool.select_backend();
    assert(selected1 != nullptr);

    // Give backend1 2 connections, backend2 1 connection
    // Effective load: backend1 = 2/2 = 1.0, backend2 = 1/1 = 1.0
    backend1->increment_connections();
    backend1->increment_connections();
    backend2->increment_connections();

    // Both have same effective load, either could be selected
    auto selected2 = pool.select_backend();
    assert(selected2 != nullptr);

    // Give backend1 one more connection
    // Effective load: backend1 = 3/2 = 1.5, backend2 = 1/1 = 1.0
    backend1->increment_connections();

    // Backend2 should be preferred now (lower effective load)
    auto selected3 = pool.select_backend();
    assert(selected3 != nullptr);
    assert(selected3->host() == "10.0.0.2");

    std::cout << "Weighted least-connections test passed!\n";
}

int main() {
    try {
        test_round_robin();
        test_least_connections();
        test_unhealthy_backend_filtering();
        test_empty_pool();
        test_weighted_round_robin();
        test_weighted_least_connections();
        std::cout << "\nAll tests passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << "\n";
        return 1;
    }
}
