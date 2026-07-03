#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "wss/affinity.hpp"
#include "wss/scheduler.hpp"
#include "wss/thread_per_core_scheduler.hpp"

namespace {

using wss::spawn;
using wss::SubmitOptions;
using wss::ThreadPerCoreScheduler;

TEST(ThreadPerCoreSchedulerTest, TaskHandlesReceiveResults) {
    ThreadPerCoreScheduler scheduler(4);

    auto handle = spawn(scheduler, [] { return 6 * 7; });
    EXPECT_EQ(handle.join().value(), 42);

    scheduler.shutdown();
}

TEST(ThreadPerCoreSchedulerTest, PanickingTasksReportAJoinErrorWithoutPoisoningThePool) {
    ThreadPerCoreScheduler scheduler(2);

    auto handle = spawn(scheduler, []() -> std::uint32_t { throw std::runtime_error("boom"); });
    auto result = handle.join();
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message.find("boom"), std::string::npos);

    auto handle2 = spawn(scheduler, [] { return 1 + 1; });
    EXPECT_EQ(handle2.join().value(), 2);
    EXPECT_EQ(scheduler.metrics().tasks_panicked, 1u);

    scheduler.shutdown();
}

// The core structural guarantee: a task assigned to core C always runs on
// the same OS thread that owns core C, for every task, no exceptions. This
// is what "0 migrations" means concretely, not just an empirical average.
TEST(ThreadPerCoreSchedulerTest, TasksNeverMigrateFromTheirAssignedCore) {
    constexpr std::size_t kCores = 4;
    ThreadPerCoreScheduler scheduler(kCores);

    // First, identify which OS thread owns each logical core index by
    // pinning one identifying task to each core.
    std::vector<std::thread::id> owner(kCores);
    for (std::size_t c = 0; c < kCores; ++c) {
        SubmitOptions opts;
        opts.core_hint = c;
        spawn(
            scheduler, [&owner, c] { owner[c] = std::this_thread::get_id(); }, opts)
            .join();
    }

    std::atomic<bool> mismatch{false};
    std::vector<wss::JoinHandle<void>> handles;
    for (int i = 0; i < 2000; ++i) {
        std::size_t c = static_cast<std::size_t>(i) % kCores;
        SubmitOptions opts;
        opts.core_hint = c;
        handles.push_back(spawn(
            scheduler,
            [&owner, &mismatch, c] {
                if (std::this_thread::get_id() != owner[c]) {
                    mismatch.store(true, std::memory_order_relaxed);
                }
            },
            opts));
    }
    for (auto& h : handles) {
        h.join();
    }

    EXPECT_FALSE(mismatch.load());
    EXPECT_EQ(scheduler.metrics().tasks_stolen, 0u) << "tasks_stolen must be structurally 0: no stealer exists";

    scheduler.shutdown();
}

TEST(ThreadPerCoreSchedulerTest, RoundRobinPlacementDistributesAcrossAllCores) {
    constexpr std::size_t kCores = 4;
    ThreadPerCoreScheduler scheduler(kCores);

    std::mutex mu;
    std::vector<std::thread::id> observed;

    std::vector<wss::JoinHandle<void>> handles;
    for (int i = 0; i < 400; ++i) {
        handles.push_back(spawn(scheduler, [&mu, &observed] {
            std::lock_guard<std::mutex> lock(mu);
            observed.push_back(std::this_thread::get_id());
        }));
    }
    for (auto& h : handles) {
        h.join();
    }

    std::vector<std::thread::id> distinct(observed.begin(), observed.end());
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    EXPECT_EQ(distinct.size(), kCores) << "round-robin submission should touch every core";

    scheduler.shutdown();
}

TEST(ThreadPerCoreSchedulerTest, AffinityBestEffortDoesNotCrash) {
    std::atomic<bool> stop{false};
    std::jthread t([&stop] {
        while (!stop.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    // Best-effort: both true and false are acceptable outcomes depending on
    // the platform, but it must never crash.
    bool pinned = wss::affinity::pin_to_core(t.native_handle(), 0);
    (void)pinned;

    stop.store(true, std::memory_order_release);
}

TEST(ThreadPerCoreSchedulerTest, ShutdownIsIdempotent) {
    ThreadPerCoreScheduler scheduler(2);
    spawn(scheduler, [] {}).join();
    scheduler.shutdown();
    scheduler.shutdown();
}

} // namespace
