#include <memory>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "wss/cancellation.hpp"
#include "wss/job.hpp"
#include "wss/join_handle.hpp"
#include "wss/metrics.hpp"
#include "wss/result.hpp"

namespace {

TEST(UniqueFunctionTest, CallsWrappedCallable) {
    int calls = 0;
    wss::Job job([&calls] { ++calls; });
    EXPECT_TRUE(static_cast<bool>(job));
    job();
    EXPECT_EQ(calls, 1);
}

TEST(UniqueFunctionTest, CapturesMoveOnlyState) {
    auto owned = std::make_unique<int>(42);
    int observed = 0;
    wss::Job job([owned = std::move(owned), &observed]() mutable { observed = *owned; });
    job();
    EXPECT_EQ(observed, 42);
}

TEST(UniqueFunctionTest, DefaultConstructedIsEmpty) {
    wss::Job job;
    EXPECT_FALSE(static_cast<bool>(job));
}

TEST(ResultTest, OkCarriesValue) {
    auto r = wss::Result<int>::ok(7);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 7);
}

TEST(ResultTest, ErrCarriesMessage) {
    auto r = wss::Result<int>::err(wss::JoinError{"boom", nullptr});
    ASSERT_FALSE(r.is_ok());
    EXPECT_EQ(r.error().message, "boom");
}

TEST(ResultTest, RethrowRaisesOriginalException) {
    std::exception_ptr ep;
    try {
        throw std::runtime_error("original");
    } catch (...) {
        ep = std::current_exception();
    }
    auto r = wss::Result<int>::err(wss::JoinError{"original", ep});
    EXPECT_THROW(r.rethrow_if_error(), std::runtime_error);
}

TEST(ResultVoidTest, OkAndErrRoundTrip) {
    auto ok = wss::Result<void>::ok();
    EXPECT_TRUE(ok.is_ok());

    auto err = wss::Result<void>::err(wss::JoinError{"bad", nullptr});
    EXPECT_FALSE(err.is_ok());
    EXPECT_EQ(err.error().message, "bad");
}

TEST(JoinHandleTest, JoinBlocksUntilSet) {
    auto [setter, handle] = wss::new_handle_pair<int>();
    EXPECT_FALSE(handle.is_finished());

    std::thread t([s = std::move(setter)]() mutable { s.set(wss::Result<int>::ok(99)); });
    auto result = handle.join();
    t.join();

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 99);
}

TEST(JoinHandleTest, VoidResultWorks) {
    auto [setter, handle] = wss::new_handle_pair<void>();
    setter.set(wss::Result<void>::ok());
    auto result = handle.join();
    EXPECT_TRUE(result.is_ok());
}

TEST(CancellationTest, StartsNotCancelled) {
    wss::CancellationToken token;
    EXPECT_FALSE(token.is_cancelled());
}

TEST(CancellationTest, CancelIsObservedThroughClonesAndContext) {
    wss::CancellationToken token;
    wss::CancellationToken clone = token;
    auto ctx = token.context();

    EXPECT_FALSE(ctx.is_cancelled());
    clone.cancel();
    EXPECT_TRUE(token.is_cancelled());
    EXPECT_TRUE(ctx.is_cancelled());
}

TEST(MetricsTest, CountersAccumulate) {
    wss::Metrics metrics;
    metrics.record_submitted();
    metrics.record_submitted();
    metrics.record_completed();
    metrics.record_panicked();
    metrics.record_stolen();

    auto snap = metrics.snapshot();
    EXPECT_EQ(snap.tasks_submitted, 2u);
    EXPECT_EQ(snap.tasks_completed, 1u);
    EXPECT_EQ(snap.tasks_panicked, 1u);
    EXPECT_EQ(snap.tasks_stolen, 1u);
}

} // namespace
