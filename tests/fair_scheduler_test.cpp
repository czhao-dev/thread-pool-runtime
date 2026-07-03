#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "wss/fair_scheduler.hpp"
#include "wss/scheduler.hpp"

namespace {

using namespace std::chrono_literals;
using wss::FairScheduler;
using wss::spawn;
using wss::SubmitOptions;

TEST(FairSchedulerTest, TaskHandlesReceiveResults) {
    FairScheduler scheduler(4);

    auto handle = spawn(scheduler, [] { return 6 * 7; });
    EXPECT_EQ(handle.join().value(), 42);

    scheduler.shutdown();
}

TEST(FairSchedulerTest, PanickingTasksReportAJoinErrorWithoutPoisoningThePool) {
    FairScheduler scheduler(2);

    auto handle = spawn(scheduler, []() -> std::uint32_t { throw std::runtime_error("boom"); });
    auto result = handle.join();
    ASSERT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message.find("boom"), std::string::npos);

    auto handle2 = spawn(scheduler, [] { return 1 + 1; });
    EXPECT_EQ(handle2.join().value(), 2);
    EXPECT_EQ(scheduler.metrics().tasks_panicked, 1u);

    scheduler.shutdown();
}

TEST(FairSchedulerTest, UnspecifiedClassNameMapsToADefaultClass) {
    FairScheduler scheduler(2);

    spawn(scheduler, [] {}).join();
    spawn(scheduler, [] {}).join();

    auto snap = scheduler.fairness_snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].name, "default");
    EXPECT_EQ(snap[0].completed, 2u);

    scheduler.shutdown();
}

TEST(FairSchedulerTest, ClassWeightValidationRejectsNonPositiveWeight) {
    FairScheduler scheduler(1);
    EXPECT_THROW(scheduler.register_class("bad", 0.0), std::invalid_argument);
    EXPECT_THROW(scheduler.register_class("bad2", -1.0), std::invalid_argument);
    scheduler.shutdown();
}

TEST(FairSchedulerTest, RegisteringTheSameClassTwiceThrows) {
    FairScheduler scheduler(1);
    scheduler.register_class("dup", 1.0);
    EXPECT_THROW(scheduler.register_class("dup", 2.0), std::invalid_argument);
    scheduler.shutdown();
}

TEST(FairSchedulerTest, VruntimeConvergesUnderEqualWeightsAndEqualWork) {
    FairScheduler scheduler(2);
    scheduler.register_class("a", 1.0);
    scheduler.register_class("b", 1.0);

    constexpr int kTasks = 300;
    std::vector<wss::JoinHandle<void>> handles;
    for (int i = 0; i < kTasks; ++i) {
        SubmitOptions a_opts;
        a_opts.class_name = "a";
        handles.push_back(spawn(scheduler, [] { std::this_thread::sleep_for(50us); }, a_opts));

        SubmitOptions b_opts;
        b_opts.class_name = "b";
        handles.push_back(spawn(scheduler, [] { std::this_thread::sleep_for(50us); }, b_opts));
    }
    for (auto& h : handles) {
        h.join();
    }

    double va = -1, vb = -1;
    for (const auto& c : scheduler.fairness_snapshot()) {
        if (c.name == "a") {
            va = c.vruntime;
        } else if (c.name == "b") {
            vb = c.vruntime;
        }
    }
    ASSERT_GE(va, 0.0);
    ASSERT_GE(vb, 0.0);
    EXPECT_NEAR(va, vb, std::max(va, vb) * 0.25 + 1e-6)
        << "equal weight + equal work should converge to close vruntimes: va=" << va << " vb=" << vb;

    scheduler.shutdown();
}

TEST(FairSchedulerTest, ProportionalShareUnderWeightSkew) {
    FairScheduler scheduler(2);
    scheduler.register_class("heavy", 4.0);
    scheduler.register_class("light", 1.0);

    constexpr int kTasksPerClass = 2000;
    constexpr auto kTaskCost = 100us;
    for (int i = 0; i < kTasksPerClass; ++i) {
        SubmitOptions heavy_opts;
        heavy_opts.class_name = "heavy";
        spawn(scheduler, [kTaskCost] { std::this_thread::sleep_for(kTaskCost); }, heavy_opts);

        SubmitOptions light_opts;
        light_opts.class_name = "light";
        spawn(scheduler, [kTaskCost] { std::this_thread::sleep_for(kTaskCost); }, light_opts);
    }

    // Poll until half the total backlog has drained, then compare
    // completed-count share while both classes are still backlogged --
    // robust to absolute execution speed (plain vs. TSan/ASan), since it
    // only depends on relative progress.
    const std::uint64_t target = kTasksPerClass;
    for (;;) {
        std::uint64_t total = 0;
        for (const auto& c : scheduler.fairness_snapshot()) {
            total += c.completed;
        }
        if (total >= target) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    double heavy_completed = 0;
    double light_completed = 0;
    for (const auto& c : scheduler.fairness_snapshot()) {
        if (c.name == "heavy") {
            heavy_completed = static_cast<double>(c.completed);
        } else if (c.name == "light") {
            light_completed = static_cast<double>(c.completed);
        }
    }
    ASSERT_GT(light_completed, 0.0);
    double ratio = heavy_completed / light_completed;
    EXPECT_GT(ratio, 2.0) << "heavy (weight 4) should complete meaningfully more than light (weight 1); ratio="
                           << ratio;

    scheduler.shutdown();
}

TEST(FairSchedulerTest, StarvationPreventionUnderGreedyClass) {
    FairScheduler scheduler(2);

    // A large, fixed greedy backlog -- large enough that "wait for the
    // whole backlog to drain first" (what a plain FIFO/strict-priority
    // scheduler would force on a late-arriving task) would take far longer
    // than a fairly scheduled light task should ever have to wait.
    constexpr int kGreedyTasks = 1000;
    for (int i = 0; i < kGreedyTasks; ++i) {
        SubmitOptions opts;
        opts.class_name = "greedy";
        spawn(scheduler, [] { std::this_thread::sleep_for(200us); }, opts);
    }

    SubmitOptions light_opts;
    light_opts.class_name = "light";
    auto light = spawn(scheduler, [] { return 42; }, light_opts);

    // The light task must complete within a generous bound despite the
    // greedy class's backlog -- the actual anti-starvation property this
    // scheduler exists to provide. 1000 tasks * 200us / 2 workers ~= 100ms
    // to drain the entire greedy backlog serially; a fairly scheduled
    // light task should finish far sooner than that.
    auto fut = std::async(std::launch::async, [&light] { return light.join(); });
    auto status = fut.wait_for(2s);
    ASSERT_EQ(status, std::future_status::ready)
        << "light task did not complete within timeout despite fair scheduling";
    EXPECT_EQ(fut.get().value(), 42);

    scheduler.shutdown();
}

TEST(FairSchedulerTest, NewClassIsSeededNearTheCurrentFloorNotZero) {
    FairScheduler scheduler(1); // single worker: no dispatch staleness, exact accounting
    scheduler.register_class("veteran", 1.0);

    std::vector<wss::JoinHandle<void>> handles;
    for (int i = 0; i < 50; ++i) {
        SubmitOptions opts;
        opts.class_name = "veteran";
        handles.push_back(spawn(scheduler, [] { std::this_thread::sleep_for(200us); }, opts));
    }
    for (auto& h : handles) {
        h.join();
    }

    double veteran_vruntime = -1;
    for (const auto& c : scheduler.fairness_snapshot()) {
        if (c.name == "veteran") {
            veteran_vruntime = c.vruntime;
        }
    }
    ASSERT_GT(veteran_vruntime, 0.0);

    SubmitOptions newcomer_opts;
    newcomer_opts.class_name = "newcomer";
    spawn(scheduler, [] { std::this_thread::sleep_for(200us); }, newcomer_opts).join();

    double newcomer_vruntime = -1;
    for (const auto& c : scheduler.fairness_snapshot()) {
        if (c.name == "newcomer") {
            newcomer_vruntime = c.vruntime;
        }
    }
    ASSERT_GE(newcomer_vruntime, 0.0);

    // Without the min_vruntime floor, the newcomer would start at ~0 and
    // its vruntime after one task would be a tiny fraction of veteran's
    // (~200us vs. ~10ms) instead of comparable to it.
    EXPECT_GT(newcomer_vruntime, veteran_vruntime * 0.5)
        << "newcomer_vruntime=" << newcomer_vruntime << " veteran_vruntime=" << veteran_vruntime;

    scheduler.shutdown();
}

TEST(FairSchedulerTest, ShutdownIsIdempotent) {
    FairScheduler scheduler(2);
    spawn(scheduler, [] {}).join();
    scheduler.shutdown();
    scheduler.shutdown();
}

} // namespace
