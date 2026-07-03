#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "wss/scheduler.hpp"
#include "wss/work_stealing_scheduler.hpp"

namespace {

using namespace std::chrono_literals;
using wss::spawn;
using wss::WorkStealingScheduler;

TEST(ShutdownTest, ShutdownWaitsForInFlightTasksToFinish) {
    WorkStealingScheduler scheduler(4);
    std::atomic<int> completed{0};

    for (int i = 0; i < 32; ++i) {
        spawn(scheduler, [&completed] {
            std::this_thread::sleep_for(5ms);
            completed.fetch_add(1, std::memory_order_seq_cst);
        });
    }

    scheduler.shutdown();
    EXPECT_EQ(completed.load(), 32);
}

TEST(ShutdownTest, ShutdownIsIdempotent) {
    WorkStealingScheduler scheduler(2);
    spawn(scheduler, [] {}).join();
    scheduler.shutdown();
    scheduler.shutdown();
}

TEST(ShutdownTest, RepeatedCreateAndShutdownCyclesAreClean) {
    for (int i = 0; i < 20; ++i) {
        WorkStealingScheduler scheduler(2);
        auto handle = spawn(scheduler, [] { return 1 + 1; });
        EXPECT_EQ(handle.join().value(), 2);
        scheduler.shutdown();
    }
}

TEST(ShutdownTest, DestroyingSchedulerWithoutExplicitShutdownStillRunsTasks) {
    std::atomic<int> completed{0};
    {
        WorkStealingScheduler scheduler(2);
        for (int i = 0; i < 10; ++i) {
            spawn(scheduler, [&completed] { completed.fetch_add(1, std::memory_order_seq_cst); });
        }
        // scheduler destroyed here without calling shutdown()
    }
    EXPECT_EQ(completed.load(), 10);
}

} // namespace
