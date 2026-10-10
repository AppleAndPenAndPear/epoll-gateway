#include <gtest/gtest.h>
#include "connection_budget.h"
#include <thread>
#include <vector>
#include <atomic>

// The budget is a process-wide ceiling shared by independent worker threads, so
// the tests focus on the two properties that make it safe under concurrency:
// it never over-admits, and a slot is returned exactly once per ticket.

TEST(ConnectionBudgetTest, AdmitsUpToLimitThenRejects) {
    ConnectionBudget budget(2);

    auto first = budget.try_acquire();
    auto second = budget.try_acquire();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(budget.in_use(), 2u);

    // At the ceiling: no more slots are handed out.
    EXPECT_FALSE(budget.try_acquire().has_value());
    EXPECT_EQ(budget.in_use(), 2u);
}

TEST(ConnectionBudgetTest, TicketReleaseReturnsSlot) {
    ConnectionBudget budget(1);
    {
        auto ticket = budget.try_acquire();
        ASSERT_TRUE(ticket.has_value());
        EXPECT_FALSE(budget.try_acquire().has_value());
    }  // ticket destroyed here
    EXPECT_EQ(budget.in_use(), 0u);
    // The slot is reusable once released.
    EXPECT_TRUE(budget.try_acquire().has_value());
}

TEST(ConnectionBudgetTest, MovedTicketReleasesExactlyOnce) {
    ConnectionBudget budget(1);
    auto original = budget.try_acquire();
    ASSERT_TRUE(original.has_value());

    // Moving must transfer ownership; the source must not release on destruction.
    ConnectionBudget::Ticket moved = std::move(*original);
    original.reset();
    EXPECT_EQ(budget.in_use(), 1u) << "moving the ticket must not release the slot";

    // The destination releases exactly once.
    moved = ConnectionBudget::Ticket{};
    EXPECT_EQ(budget.in_use(), 0u);
}

TEST(ConnectionBudgetTest, SetLimitLowerThanUsageDoesNotEvict) {
    ConnectionBudget budget(4);
    auto a = budget.try_acquire();
    auto b = budget.try_acquire();
    ASSERT_TRUE(a && b);
    EXPECT_EQ(budget.in_use(), 2u);

    // Lowering below the current usage keeps live connections but blocks new ones.
    budget.set_limit(1);
    EXPECT_EQ(budget.limit(), 1u);
    EXPECT_EQ(budget.in_use(), 2u);
    EXPECT_FALSE(budget.try_acquire().has_value());

    // Releasing one slot still leaves usage at the ceiling, so admission stays
    // blocked until the count is strictly below the limit.
    a.reset();
    EXPECT_EQ(budget.in_use(), 1u);
    EXPECT_FALSE(budget.try_acquire().has_value());

    // Once usage drops below the ceiling, admission resumes.
    b.reset();
    EXPECT_TRUE(budget.try_acquire().has_value());
}

TEST(ConnectionBudgetTest, ConcurrentAcquireNeverExceedsLimit) {
    constexpr size_t kLimit = 32;
    constexpr int kThreads = 8;
    ConnectionBudget budget(kLimit);
    std::atomic<bool> start{false};
    std::atomic<size_t> peak{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            while (!start.load()) {
            }
            // Each thread greedily holds every slot it can; the shared count must
            // never rise above the limit no matter how the threads interleave.
            std::vector<ConnectionBudget::Ticket> held;
            while (auto ticket = budget.try_acquire()) {
                held.push_back(std::move(*ticket));
                const size_t current = budget.in_use();
                size_t observed = peak.load();
                while (current > observed &&
                       !peak.compare_exchange_weak(observed, current)) {
                }
            }
        });
    }
    start.store(true);
    for (auto& t : threads) t.join();

    // All slots are released when the threads' tickets go out of scope.
    EXPECT_EQ(budget.in_use(), 0u);
    EXPECT_LE(peak.load(), kLimit) << "the budget admitted more slots than the limit";
    EXPECT_GT(peak.load(), 0u) << "no thread managed to acquire a slot";
}