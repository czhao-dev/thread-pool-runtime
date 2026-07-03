#include "wss/global_queue_scheduler.hpp"

#include <stdexcept>

namespace wss {

GlobalQueueScheduler::GlobalQueueScheduler(std::size_t num_workers) : num_workers_(num_workers) {
    if (num_workers == 0) {
        throw std::invalid_argument("a scheduler needs at least one worker thread");
    }

    workers_.reserve(num_workers);
    for (std::size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

GlobalQueueScheduler::~GlobalQueueScheduler() { shutdown(); }

void GlobalQueueScheduler::worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            for (;;) {
                if (!queue_.empty()) {
                    job = std::move(queue_.front());
                    queue_.pop_front();
                    break;
                }
                if (shutdown_flag_.load(std::memory_order_acquire)) {
                    return;
                }
                cv_.wait(lock);
            }
        }
        job();
        metrics_.record_completed();
    }
}

void GlobalQueueScheduler::submit(Job job, const SubmitOptions& /*opts*/) {
    metrics_.record_submitted();
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void GlobalQueueScheduler::shutdown() {
    shutdown_flag_.store(true, std::memory_order_seq_cst);
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

RuntimeMetrics GlobalQueueScheduler::metrics() const { return metrics_.snapshot(); }

std::size_t GlobalQueueScheduler::worker_count() const { return num_workers_; }

void GlobalQueueScheduler::record_panic() noexcept { metrics_.record_panicked(); }

} // namespace wss
