#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "wss/cancellation.hpp"
#include "wss/scheduler.hpp"
#include "wss/work_stealing_scheduler.hpp"

namespace {

using namespace std::chrono_literals;
using wss::CancellationToken;
using wss::spawn_cancellable;
using wss::WorkStealingScheduler;

TEST(CancellationTest, CancelledTasksObserveCancellation) {
    WorkStealingScheduler scheduler(2);
    CancellationToken token;
    std::atomic<int> iterations{0};

    auto handle = spawn_cancellable(scheduler, token, [&iterations](const wss::CancellationContext& ctx) {
        while (!ctx.is_cancelled()) {
            iterations.fetch_add(1, std::memory_order_seq_cst);
            std::this_thread::sleep_for(1ms);
        }
        return std::string("stopped cooperatively");
    });

    std::this_thread::sleep_for(20ms);
    token.cancel();

    EXPECT_EQ(handle.join().value(), "stopped cooperatively");
    EXPECT_GT(iterations.load(), 0);

    scheduler.shutdown();
}

TEST(CancellationTest, CancellingBeforeTheTaskStartsIsObservedImmediately) {
    WorkStealingScheduler scheduler(1);
    CancellationToken token;
    token.cancel();

    auto handle =
        spawn_cancellable(scheduler, token, [](const wss::CancellationContext& ctx) { return ctx.is_cancelled(); });
    EXPECT_TRUE(handle.join().value());

    scheduler.shutdown();
}

TEST(CancellationTest, UnrelatedTasksAreUnaffectedByACancelledToken) {
    WorkStealingScheduler scheduler(2);
    CancellationToken token;
    token.cancel();

    spawn_cancellable(scheduler, token, [](const wss::CancellationContext&) {}).join();

    auto handle = wss::spawn(scheduler, [] { return 5 * 5; });
    EXPECT_EQ(handle.join().value(), 25);

    scheduler.shutdown();
}

} // namespace
