#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "wss/global_queue_scheduler.hpp"
#include "wss/scheduler.hpp"

namespace {

using namespace std::chrono_literals;
using wss::GlobalQueueScheduler;
using wss::spawn;

TEST(GlobalQueueSchedulerTest, TaskHandlesReceiveResults) {
    GlobalQueueScheduler scheduler(4);

    auto handle = spawn(scheduler, [] { return 6 * 7; });
    EXPECT_EQ(handle.join().value(), 42);

    scheduler.shutdown();
}

TEST(GlobalQueueSchedulerTest, PanickingTasksReportAJoinErrorWithoutPoisoningThePool) {
    GlobalQueueScheduler scheduler(2);

    auto handle = spawn(scheduler, []() -> std::uint32_t { throw std::runtime_error("boom"); });
    auto result = handle.join();
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message.find("boom"), std::string::npos);

    auto handle2 = spawn(scheduler, [] { return 1 + 1; });
    EXPECT_EQ(handle2.join().value(), 2);
    EXPECT_EQ(scheduler.metrics().tasks_panicked, 1u);

    scheduler.shutdown();
}

TEST(GlobalQueueSchedulerTest, SubmissionOrderIsFifoOnASingleWorker) {
    GlobalQueueScheduler scheduler(1);
    std::mutex mu;
    std::vector<int> order;

    std::vector<wss::JoinHandle<void>> handles;
    for (int i = 0; i < 100; ++i) {
        handles.push_back(spawn(scheduler, [&mu, &order, i] {
            std::lock_guard<std::mutex> lock(mu);
            order.push_back(i);
        }));
    }
    for (auto& h : handles) {
        h.join();
    }

    ASSERT_EQ(order.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
    }

    scheduler.shutdown();
}

TEST(GlobalQueueSchedulerTest, ShutdownWaitsForQueuedTasksToDrain) {
    GlobalQueueScheduler scheduler(4);
    std::atomic<int> completed{0};

    for (int i = 0; i < 200; ++i) {
        spawn(scheduler, [&completed] {
            std::this_thread::sleep_for(1ms);
            completed.fetch_add(1, std::memory_order_seq_cst);
        });
    }

    scheduler.shutdown();
    EXPECT_EQ(completed.load(), 200);
}

TEST(GlobalQueueSchedulerTest, ShutdownIsIdempotent) {
    GlobalQueueScheduler scheduler(2);
    spawn(scheduler, [] {}).join();
    scheduler.shutdown();
    scheduler.shutdown();
}

TEST(GlobalQueueSchedulerTest, MetricsTrackSubmittedAndCompletedTasks) {
    GlobalQueueScheduler scheduler(4);

    for (int i = 0; i < 50; ++i) {
        spawn(scheduler, [] {}).join();
    }

    auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, 50u);
    EXPECT_EQ(metrics.tasks_completed, 50u);
    EXPECT_EQ(metrics.tasks_stolen, 0u);

    scheduler.shutdown();
}

} // namespace
