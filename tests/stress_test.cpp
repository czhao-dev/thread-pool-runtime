#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "wss/priority.hpp"
#include "wss/scheduler.hpp"
#include "wss/work_stealing_scheduler.hpp"

namespace {

using namespace std::chrono_literals;
using wss::Priority;
using wss::spawn;
using wss::SubmitOptions;
using wss::WorkStealingScheduler;

TEST(StressTest, ManyConcurrentSubmissionsFromMultipleThreads) {
    WorkStealingScheduler scheduler(8);
    std::atomic<int> completed{0};

    std::vector<std::thread> submitters;
    for (int s = 0; s < 8; ++s) {
        submitters.emplace_back([&scheduler, &completed] {
            std::vector<wss::JoinHandle<void>> handles;
            for (int i = 0; i < 500; ++i) {
                handles.push_back(
                    spawn(scheduler, [&completed] { completed.fetch_add(1, std::memory_order_seq_cst); }));
            }
            for (auto& h : handles) {
                h.join();
            }
        });
    }
    for (auto& t : submitters) {
        t.join();
    }

    EXPECT_EQ(completed.load(), 4000);
    scheduler.shutdown();
}

TEST(StressTest, LongRunningTasksMixedWithShortTasks) {
    WorkStealingScheduler scheduler(4);

    auto long_task = spawn(scheduler, [] {
        std::this_thread::sleep_for(50ms);
        return std::string("long done");
    });

    std::vector<wss::JoinHandle<int>> shorts;
    for (int i = 0; i < 2000; ++i) {
        shorts.push_back(spawn(scheduler, [i] { return i * 2; }));
    }
    for (int i = 0; i < 2000; ++i) {
        EXPECT_EQ(shorts[static_cast<std::size_t>(i)].join().value(), i * 2);
    }
    EXPECT_EQ(long_task.join().value(), "long done");

    scheduler.shutdown();
}

TEST(StressTest, PriorityTasksArePreferredOverBackgroundWork) {
    WorkStealingScheduler scheduler(1);
    std::atomic<int> order{0};
    std::atomic<bool> release{false};

    // Gate the single worker on a first background task that blocks until
    // every task below (background and high) has actually been submitted.
    // Without this, whether the priority scan "wins" depends on a race
    // between the submitting thread and however fast the worker drains
    // batches off the Background injector — fine most of the time, but
    // flaky under heavy instrumentation (observed failing under TSan).
    // Gating makes the scheduling guarantee this test checks deterministic.
    SubmitOptions gate_opts;
    gate_opts.priority = Priority::Background;
    auto gate = spawn(
        scheduler,
        [&release] {
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        },
        gate_opts);

    std::vector<wss::JoinHandle<int>> background_handles;
    for (int i = 0; i < 500; ++i) {
        SubmitOptions opts;
        opts.priority = Priority::Background;
        background_handles.push_back(
            spawn(scheduler, [&order] { return order.fetch_add(1, std::memory_order_seq_cst); }, opts));
    }

    SubmitOptions high_opts;
    high_opts.priority = Priority::High;
    auto high = spawn(scheduler, [&order] { return order.fetch_add(1, std::memory_order_seq_cst); }, high_opts);

    release.store(true, std::memory_order_release);
    gate.join();

    int high_rank = high.join().value();
    for (auto& h : background_handles) {
        h.join();
    }

    EXPECT_LT(high_rank, 500) << "high-priority task ran at position " << high_rank
                               << ", expected it to cut ahead of most background work";

    scheduler.shutdown();
}

TEST(StressTest, RepeatedPoolCreateAndShutdownUnderLoad) {
    for (int cycle = 0; cycle < 10; ++cycle) {
        WorkStealingScheduler scheduler(4);
        std::vector<wss::JoinHandle<int>> handles;
        for (int i = 0; i < 200; ++i) {
            handles.push_back(spawn(scheduler, [i] { return i + 1; }));
        }
        for (int i = 0; i < 200; ++i) {
            EXPECT_EQ(handles[static_cast<std::size_t>(i)].join().value(), i + 1);
        }
        scheduler.shutdown();
    }
}

} // namespace
