#include "core/memory_budget.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

using lb::core::MemoryBudget;

void test_defaults() {
    MemoryBudget budget;
    assert(budget.limit_bytes() == 0);
    assert(budget.used_bytes() == 0);
    assert(!budget.is_exceeded());
    // Zero limit means unlimited — reserve always succeeds
    assert(budget.try_reserve(999999));
    std::cout << "  defaults: PASS\n";
}

void test_set_limit_mb() {
    MemoryBudget budget;
    budget.set_limit_mb(2);
    assert(budget.limit_bytes() == 2ULL * 1024 * 1024);
    std::cout << "  set_limit_mb: PASS\n";
}

void test_set_limit_bytes() {
    MemoryBudget budget;
    budget.set_limit_bytes(65536);
    assert(budget.limit_bytes() == 65536);
    std::cout << "  set_limit_bytes: PASS\n";
}

void test_reserve_and_release() {
    MemoryBudget budget;
    budget.set_limit_bytes(1024);

    assert(budget.try_reserve(512));
    assert(budget.used_bytes() == 512);
    assert(!budget.is_exceeded());

    assert(budget.try_reserve(512));
    assert(budget.used_bytes() == 1024);
    assert(budget.is_exceeded()); // 1024 >= 1024

    budget.release(256);
    assert(budget.used_bytes() == 768);
    assert(!budget.is_exceeded()); // 768 < 1024

    budget.release(768);
    assert(budget.used_bytes() == 0);
    assert(!budget.is_exceeded());

    std::cout << "  reserve_and_release: PASS\n";
}

void test_reserve_fails_when_exceeded() {
    MemoryBudget budget;
    budget.set_limit_bytes(100);

    assert(budget.try_reserve(100));
    assert(budget.is_exceeded());

    // Further reserves should fail (100 + 1 > 100)
    assert(!budget.try_reserve(1));
    assert(!budget.try_reserve(100));
    assert(budget.used_bytes() == 100); // unchanged

    budget.release(50);
    // Now 50 + 1 <= 100? No: 50 + 1 > 100 is false, so reserve succeeds
    // try_reserve checks cur + n > lim → 50 + 1 = 51 > 100 → false → succeeds
    assert(budget.try_reserve(1));
    assert(budget.used_bytes() == 51);

    // But 51 + 50 = 101 > 100 → fails
    assert(!budget.try_reserve(50));
    assert(budget.used_bytes() == 51);

    std::cout << "  reserve_fails_when_exceeded: PASS\n";
}

void test_release_underflow_clamps_to_zero() {
    MemoryBudget budget;
    budget.set_limit_bytes(1024);

    assert(budget.try_reserve(10));
    budget.release(9999); // releasing more than used
    assert(budget.used_bytes() == 0); // should clamp to 0, not underflow

    std::cout << "  release_underflow_clamps: PASS\n";
}

void test_zero_reserve_always_succeeds() {
    MemoryBudget budget;
    budget.set_limit_bytes(0);
    assert(budget.try_reserve(0));

    budget.set_limit_bytes(100);
    assert(budget.try_reserve(0)); // zero-byte reserve always ok
    assert(budget.used_bytes() == 0);

    std::cout << "  zero_reserve: PASS\n";
}

void test_zero_release_is_noop() {
    MemoryBudget budget;
    budget.set_limit_bytes(100);
    assert(budget.try_reserve(50));
    budget.release(0);
    assert(budget.used_bytes() == 50);

    std::cout << "  zero_release: PASS\n";
}

void test_concurrent_reserves() {
    MemoryBudget budget;
    budget.set_limit_bytes(1000000); // 1 MB

    constexpr int nthreads = 8;
    constexpr int ops = 10000;
    std::vector<std::thread> threads;

    for (int i = 0; i < nthreads; ++i) {
        threads.emplace_back([&budget]() {
            for (int j = 0; j < ops; ++j) {
                if (budget.try_reserve(1)) {
                    budget.release(1);
                }
            }
        });
    }
    for (auto& t : threads)
        t.join();

    // After all threads finish, used should be 0
    assert(budget.used_bytes() == 0);

    std::cout << "  concurrent_reserves: PASS\n";
}

void test_is_exceeded_boundary() {
    MemoryBudget budget;
    budget.set_limit_bytes(64);

    // Reserve exactly limit: succeeds (0 + 64 > 64 is false)
    assert(budget.try_reserve(64));
    assert(budget.used_bytes() == 64);
    assert(budget.is_exceeded()); // 64 >= 64

    // One more byte fails
    assert(!budget.try_reserve(1));

    budget.release(1);
    assert(budget.used_bytes() == 63);
    assert(!budget.is_exceeded()); // 63 < 64

    std::cout << "  is_exceeded_boundary: PASS\n";
}

int main() {
    std::cout << "MemoryBudget unit tests:\n";
    test_defaults();
    test_set_limit_mb();
    test_set_limit_bytes();
    test_reserve_and_release();
    test_reserve_fails_when_exceeded();
    test_release_underflow_clamps_to_zero();
    test_zero_reserve_always_succeeds();
    test_zero_release_is_noop();
    test_concurrent_reserves();
    test_is_exceeded_boundary();
    std::cout << "All MemoryBudget tests passed!\n";
    return 0;
}
