#include <atomic>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "wss/chase_lev_deque.hpp"

namespace {

using wss::ChaseLevDeque;

TEST(ChaseLevDequeTest, PushPopIsLifoForOwner) {
    ChaseLevDeque<int> deque;
    deque.push(1);
    deque.push(2);
    deque.push(3);

    EXPECT_EQ(deque.pop(), std::optional<int>(3));
    EXPECT_EQ(deque.pop(), std::optional<int>(2));
    EXPECT_EQ(deque.pop(), std::optional<int>(1));
    EXPECT_EQ(deque.pop(), std::nullopt);
}

TEST(ChaseLevDequeTest, StealTakesFromTheOppositeEnd) {
    ChaseLevDeque<int> deque;
    for (int i = 0; i < 5; ++i) {
        deque.push(i);
    }
    // Steals come from the top (oldest pushed), pops from the bottom (newest).
    EXPECT_EQ(deque.steal(), std::optional<int>(0));
    EXPECT_EQ(deque.steal(), std::optional<int>(1));
    EXPECT_EQ(deque.pop(), std::optional<int>(4));
}

TEST(ChaseLevDequeTest, EmptyDequeReturnsNulloptFromAllOperations) {
    ChaseLevDeque<int> deque;
    EXPECT_EQ(deque.pop(), std::nullopt);
    EXPECT_EQ(deque.steal(), std::nullopt);
}

TEST(ChaseLevDequeTest, GrowsPastInitialCapacityWithoutLosingItems) {
    ChaseLevDeque<int> deque(4);
    constexpr int kCount = 5000;
    for (int i = 0; i < kCount; ++i) {
        deque.push(i);
    }

    std::vector<int> popped;
    while (auto item = deque.pop()) {
        popped.push_back(*item);
    }
    ASSERT_EQ(popped.size(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(popped[static_cast<std::size_t>(i)], kCount - 1 - i);
    }
}

TEST(ChaseLevDequeTest, DestructorFreesUndrainedItemsWithoutLeaking) {
    // No direct leak assertion without extra tooling; this just needs to
    // run cleanly (and cleanly under ASan) without crashing or double-freeing.
    ChaseLevDeque<int> deque;
    for (int i = 0; i < 100; ++i) {
        deque.push(i);
    }
}

// Every item pushed by the owner is eventually observed exactly once,
// whether via the owner's own pop() or a peer's steal() — this is the
// core work-stealing safety property, exercised under real concurrency.
TEST(ChaseLevDequeTest, ConcurrentStealersAndOwnerNeverDuplicateOrDropItems) {
    constexpr int kItems = 200'000;
    constexpr int kStealers = 8;

    ChaseLevDeque<int> deque;
    std::atomic<bool> stop{false};
    std::vector<std::vector<int>> stolen(kStealers);
    std::vector<std::thread> stealers;

    for (int s = 0; s < kStealers; ++s) {
        stealers.emplace_back([&deque, &stop, &stolen, s] {
            while (!stop.load(std::memory_order_acquire)) {
                if (auto item = deque.steal()) {
                    stolen[static_cast<std::size_t>(s)].push_back(*item);
                }
            }
            // Drain whatever's left after the owner signals completion.
            while (auto item = deque.steal()) {
                stolen[static_cast<std::size_t>(s)].push_back(*item);
            }
        });
    }

    std::vector<int> popped;
    for (int i = 0; i < kItems; ++i) {
        deque.push(i);
        if (i % 4 == 0) {
            if (auto item = deque.pop()) {
                popped.push_back(*item);
            }
        }
    }
    while (auto item = deque.pop()) {
        popped.push_back(*item);
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : stealers) {
        t.join();
    }

    std::unordered_set<int> seen(popped.begin(), popped.end());
    std::size_t total = popped.size();
    for (auto& v : stolen) {
        for (int item : v) {
            EXPECT_TRUE(seen.insert(item).second) << "item " << item << " observed more than once";
        }
        total += v.size();
    }
    EXPECT_EQ(total, static_cast<std::size_t>(kItems));
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(kItems));
}

} // namespace
