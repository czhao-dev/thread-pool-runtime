#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "wss/job.hpp"
#include "wss/metrics.hpp"
#include "wss/priority.hpp"
#include "wss/scheduler.hpp"

namespace wss {

// N OS threads, each pinned (best-effort, see affinity.hpp) to a distinct
// core, each owning a PRIVATE per-priority queue set. A task is assigned to
// a core at submit time (opts.core_hint, or round-robin if unset) and
// NEVER migrates afterward: there is no stealer concept at all, so
// "0 migrations" (metrics().tasks_stolen == 0) is a structural guarantee,
// not a policy that could be violated by a bug elsewhere.
//
// This is the classic thread-per-core design (Seastar/Glommio-style):
// maximizes cache locality for evenly-distributed work, and is expected to
// suffer badly under skewed workloads since nothing rebalances it — that
// contrast with WorkStealingScheduler is the point of having both.
class ThreadPerCoreScheduler final : public Scheduler {
public:
    explicit ThreadPerCoreScheduler(std::size_t num_cores);
    ~ThreadPerCoreScheduler() override;

    ThreadPerCoreScheduler(const ThreadPerCoreScheduler&) = delete;
    ThreadPerCoreScheduler& operator=(const ThreadPerCoreScheduler&) = delete;

    // Reads opts.core_hint if present (wrapped into [0, num_cores) via
    // modulo); otherwise assigns round-robin. Also reads opts.priority —
    // near-zero-cost to support (3 sub-queues per core already) and keeps
    // the API consistent with the other backends.
    void submit(Job job, const SubmitOptions& opts) override;

    // Idempotent: stops accepting the premise that more work is coming,
    // drains every core's queue, then joins all worker threads.
    void shutdown() override;

    RuntimeMetrics metrics() const override;
    std::size_t worker_count() const override;
    void record_panic() noexcept override;
    void record_completed() noexcept override;

private:
    struct CoreQueue {
        std::mutex mu;
        std::condition_variable cv;
        std::array<std::deque<Job>, kPriorityCount> queues;
    };

    void worker_loop(std::size_t core_index);

    std::vector<std::unique_ptr<CoreQueue>> cores_;
    std::atomic<std::size_t> next_{0};
    std::atomic<bool> shutdown_flag_{false};
    Metrics metrics_;
    std::vector<std::jthread> workers_;
    std::size_t num_cores_;
};

} // namespace wss
