#include <atomic>

#include <gtest/gtest.h>

#include "wss/chase_lev_deque.hpp"
#include "wss/injector.hpp"
#include "wss/job.hpp"

namespace {

TEST(InjectorTest, PopBatchIntoReturnsNulloptWhenEmpty) {
    wss::Injector injector;
    wss::ChaseLevDeque<wss::Job> dest;
    EXPECT_FALSE(injector.pop_batch_into(dest, 8).has_value());
}

TEST(InjectorTest, PopBatchIntoReturnsSingleJobDirectly) {
    wss::Injector injector;
    std::atomic<int> ran{0};
    injector.push(wss::Job([&ran] { ran.fetch_add(1); }));

    wss::ChaseLevDeque<wss::Job> dest;
    auto job = injector.pop_batch_into(dest, 8);
    ASSERT_TRUE(job.has_value());
    (*job)();
    EXPECT_EQ(ran.load(), 1);
    EXPECT_FALSE(dest.pop().has_value());
}

TEST(InjectorTest, PopBatchIntoMovesRemainderIntoDestUpToLimit) {
    wss::Injector injector;
    std::atomic<int> ran{0};
    for (int i = 0; i < 10; ++i) {
        injector.push(wss::Job([&ran] { ran.fetch_add(1); }));
    }

    wss::ChaseLevDeque<wss::Job> dest;
    auto first = injector.pop_batch_into(dest, 4);
    ASSERT_TRUE(first.has_value());
    (*first)();

    int drained = 0;
    while (auto job = dest.pop()) {
        (*job)();
        ++drained;
    }
    // max_batch=4 means 1 returned directly + up to 3 moved into dest.
    EXPECT_EQ(drained, 3);
    EXPECT_EQ(ran.load(), 4);

    // The remaining 6 jobs are still queued in the injector itself.
    int remaining = 0;
    while (auto job = injector.pop_batch_into(dest, 1)) {
        (*job)();
        ++remaining;
    }
    EXPECT_EQ(remaining, 6);
    EXPECT_EQ(ran.load(), 10);
}

} // namespace
