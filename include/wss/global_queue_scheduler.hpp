#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "wss/job.hpp"
#include "wss/metrics.hpp"
#include "wss/scheduler.hpp"

namespace wss {

// A plain shared-mutex thread pool: one mutex-guarded FIFO queue, no local
// queues, no stealing. Exists purely as the "naive baseline" the other
// three schedulers are measured against in the benchmark suite.
//
// Deliberately keeps its own plain condition_variable doorbell rather than
// adopting IdleSignal — part of this baseline's value is contrasting
// "simplest possible design" against the more sophisticated doorbell used
// elsewhere; sharing it would blur that intentional contrast.
class GlobalQueueScheduler final : public Scheduler {
public:
    explicit GlobalQueueScheduler(std::size_t num_workers);
    ~GlobalQueueScheduler() override;

    GlobalQueueScheduler(const GlobalQueueScheduler&) = delete;
    GlobalQueueScheduler& operator=(const GlobalQueueScheduler&) = delete;

    // Ignores opts entirely — plain submission-order FIFO.
    void submit(Job job, const SubmitOptions& opts) override;

    // Idempotent: stops accepting the premise that more work is coming,
    // drains the queue, then joins all worker threads.
    void shutdown() override;

    RuntimeMetrics metrics() const override;
    std::size_t worker_count() const override;
    void record_panic() noexcept override;
    void record_completed() noexcept override;

private:
    void worker_loop();

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::atomic<bool> shutdown_flag_{false};
    Metrics metrics_;
    std::vector<std::jthread> workers_;
    std::size_t num_workers_;
};

} // namespace wss
