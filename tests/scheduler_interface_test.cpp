#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "wss/fair_scheduler.hpp"
#include "wss/global_queue_scheduler.hpp"
#include "wss/scheduler.hpp"
#include "wss/thread_per_core_scheduler.hpp"
#include "wss/work_stealing_scheduler.hpp"

namespace {

using wss::Scheduler;
using wss::spawn;

constexpr std::size_t kWorkers = 4;

struct WorkStealingFactory {
    static std::unique_ptr<Scheduler> create() { return std::make_unique<wss::WorkStealingScheduler>(kWorkers); }
};
struct GlobalQueueFactory {
    static std::unique_ptr<Scheduler> create() { return std::make_unique<wss::GlobalQueueScheduler>(kWorkers); }
};
struct ThreadPerCoreFactory {
    static std::unique_ptr<Scheduler> create() { return std::make_unique<wss::ThreadPerCoreScheduler>(kWorkers); }
};
struct FairFactory {
    static std::unique_ptr<Scheduler> create() { return std::make_unique<wss::FairScheduler>(kWorkers); }
};

template <class Factory>
class SchedulerInterfaceTest : public ::testing::Test {
protected:
    std::unique_ptr<Scheduler> scheduler = Factory::create();
};

using SchedulerFactories =
    ::testing::Types<WorkStealingFactory, GlobalQueueFactory, ThreadPerCoreFactory, FairFactory>;

TYPED_TEST_SUITE(SchedulerInterfaceTest, SchedulerFactories);

TYPED_TEST(SchedulerInterfaceTest, SubmittedTaskRunsAndJoinReturnsResult) {
    auto handle = spawn(*this->scheduler, [] { return 6 * 7; });
    EXPECT_EQ(handle.join().value(), 42);
    this->scheduler->shutdown();
}

TYPED_TEST(SchedulerInterfaceTest, PanickingTaskReportsJoinErrorWithoutPoisoningThePool) {
    auto handle = spawn(*this->scheduler, []() -> int { throw std::runtime_error("boom"); });
    auto result = handle.join();
    EXPECT_FALSE(result.is_ok());
    EXPECT_NE(result.error().message.find("boom"), std::string::npos);

    auto handle2 = spawn(*this->scheduler, [] { return 1; });
    EXPECT_EQ(handle2.join().value(), 1);
    EXPECT_EQ(this->scheduler->metrics().tasks_panicked, 1u);

    this->scheduler->shutdown();
}

TYPED_TEST(SchedulerInterfaceTest, MetricsTrackSubmittedAndCompletedTasks) {
    for (int i = 0; i < 20; ++i) {
        spawn(*this->scheduler, [] {}).join();
    }
    auto m = this->scheduler->metrics();
    EXPECT_EQ(m.tasks_submitted, 20u);
    EXPECT_EQ(m.tasks_completed, 20u);
    this->scheduler->shutdown();
}

TYPED_TEST(SchedulerInterfaceTest, WorkerCountMatchesConstruction) {
    EXPECT_EQ(this->scheduler->worker_count(), kWorkers);
    this->scheduler->shutdown();
}

TYPED_TEST(SchedulerInterfaceTest, ShutdownIsIdempotent) {
    spawn(*this->scheduler, [] {}).join();
    this->scheduler->shutdown();
    this->scheduler->shutdown();
}

TYPED_TEST(SchedulerInterfaceTest, HighFanoutCompletesThroughTheInterface) {
    std::atomic<int> completed{0};
    std::vector<wss::JoinHandle<void>> handles;
    for (int i = 0; i < 2000; ++i) {
        handles.push_back(
            spawn(*this->scheduler, [&completed] { completed.fetch_add(1, std::memory_order_seq_cst); }));
    }
    for (auto& h : handles) {
        h.join();
    }
    EXPECT_EQ(completed.load(), 2000);
    this->scheduler->shutdown();
}

} // namespace
