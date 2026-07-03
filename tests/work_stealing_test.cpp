#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "wss/scheduler.hpp"
#include "wss/work_stealing_scheduler.hpp"

namespace {

using wss::spawn;
using wss::WorkStealingScheduler;

TEST(WorkStealingTest, IdleWorkersKeepMakingProgressWhileOneWorkerIsBusy) {
    WorkStealingScheduler scheduler(4);

    std::atomic<bool> release{false};
    auto blocker = spawn(scheduler, [&release] {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::atomic<int> completed{0};
    auto start = std::chrono::steady_clock::now();

    std::vector<wss::JoinHandle<void>> handles;
    for (int i = 0; i < 300; ++i) {
        handles.push_back(spawn(scheduler, [&completed] { completed.fetch_add(1, std::memory_order_seq_cst); }));
    }
    for (auto& h : handles) {
        h.join();
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    release.store(true, std::memory_order_release);
    blocker.join();

    EXPECT_EQ(completed.load(), 300);
    EXPECT_LT(elapsed, std::chrono::seconds(5))
        << "300 trivial tasks took too long despite 3 free workers; work is not being distributed";

    scheduler.shutdown();
}

TEST(WorkStealingTest, HighFanoutWorkloadIsDistributedAcrossWorkersAndCompletes) {
    WorkStealingScheduler scheduler(8);
    std::atomic<int> completed{0};

    std::vector<wss::JoinHandle<void>> handles;
    for (int i = 0; i < 20000; ++i) {
        handles.push_back(spawn(scheduler, [&completed] { completed.fetch_add(1, std::memory_order_seq_cst); }));
    }
    for (auto& h : handles) {
        h.join();
    }

    EXPECT_EQ(completed.load(), 20000);

    auto metrics = scheduler.metrics();
    EXPECT_EQ(metrics.tasks_submitted, 20000u);
    EXPECT_EQ(metrics.tasks_completed, 20000u);

    scheduler.shutdown();
}

TEST(WorkStealingTest, NestedFanoutFromWithinATaskCompletes) {
    WorkStealingScheduler scheduler(4);
    std::atomic<int> completed{0};

    auto top = spawn(scheduler, [&scheduler, &completed] {
        std::vector<wss::JoinHandle<void>> handles;
        for (int i = 0; i < 200; ++i) {
            handles.push_back(spawn(scheduler, [&completed] { completed.fetch_add(1, std::memory_order_seq_cst); }));
        }
        for (auto& h : handles) {
            h.join();
        }
    });

    top.join();
    EXPECT_EQ(completed.load(), 200);

    scheduler.shutdown();
}

} // namespace
