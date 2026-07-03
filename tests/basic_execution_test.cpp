#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "wss/scheduler.hpp"
#include "wss/work_stealing_scheduler.hpp"

namespace {

using wss::spawn;
using wss::WorkStealingScheduler;

TEST(BasicExecutionTest, SubmittedTasksEventuallyRun) {
    WorkStealingScheduler scheduler(4);
    std::atomic<int> counter{0};

    std::vector<wss::JoinHandle<void>> handles;
    for (int i = 0; i < 500; ++i) {
        handles.push_back(spawn(scheduler, [&counter] { counter.fetch_add(1, std::memory_order_seq_cst); }));
    }
    for (auto& h : handles) {
        h.join();
    }

    EXPECT_EQ(counter.load(), 500);
    scheduler.shutdown();
}

TEST(BasicExecutionTest, TaskHandlesReceiveResults) {
    WorkStealingScheduler scheduler(4);

    auto handle1 = spawn(scheduler, [] { return 6 * 7; });
    EXPECT_EQ(handle1.join().value(), 42);

    auto handle2 = spawn(scheduler, [] { return std::string("value"); });
    EXPECT_EQ(handle2.join().value(), "value");

    scheduler.shutdown();
}

TEST(BasicExecutionTest, PanickingTasksReportAJoinErrorWithoutPoisoningThePool) {
    WorkStealingScheduler scheduler(2);

    auto handle = spawn(scheduler, []() -> std::uint32_t { throw std::runtime_error("boom"); });
    auto result = handle.join();
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message.find("boom"), std::string::npos);

    // The pool must still be healthy after a panic.
    auto handle2 = spawn(scheduler, [] { return 1 + 1; });
    EXPECT_EQ(handle2.join().value(), 2);

    EXPECT_EQ(scheduler.metrics().tasks_panicked, 1u);

    scheduler.shutdown();
}

TEST(BasicExecutionTest, MetricsTrackSubmittedAndCompletedTasks) {
    WorkStealingScheduler scheduler(4);

    for (int i = 0; i < 50; ++i) {
        spawn(scheduler, [] {}).join();
    }

    auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, 50u);
    EXPECT_EQ(metrics.tasks_completed, 50u);
    EXPECT_EQ(metrics.tasks_panicked, 0u);

    scheduler.shutdown();
}

TEST(BasicExecutionTest, NestedTaskSpawningCompletes) {
    WorkStealingScheduler scheduler(4);

    auto handle = spawn(scheduler, [&scheduler] {
        auto child = spawn(scheduler, [] { return 41; });
        return child.join().value() + 1;
    });

    EXPECT_EQ(handle.join().value(), 42);
    scheduler.shutdown();
}

} // namespace
