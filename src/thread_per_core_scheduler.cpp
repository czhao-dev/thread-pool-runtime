#include "wss/thread_per_core_scheduler.hpp"

#include <stdexcept>

#include "wss/affinity.hpp"

namespace wss {

ThreadPerCoreScheduler::ThreadPerCoreScheduler(std::size_t num_cores) : num_cores_(num_cores) {
    if (num_cores == 0) {
        throw std::invalid_argument("a scheduler needs at least one worker thread");
    }

    cores_.resize(num_cores);
    for (auto& core : cores_) {
        core = std::make_unique<CoreQueue>();
    }

    workers_.reserve(num_cores);
    for (std::size_t i = 0; i < num_cores; ++i) {
        workers_.emplace_back([this, i] { worker_loop(i); });
        affinity::pin_to_core(workers_.back().native_handle(), static_cast<unsigned>(i));
    }
}

ThreadPerCoreScheduler::~ThreadPerCoreScheduler() { shutdown(); }

void ThreadPerCoreScheduler::worker_loop(std::size_t core_index) {
    CoreQueue& core = *cores_[core_index];
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(core.mu);
            for (;;) {
                bool found = false;
                for (Priority p : kPriorityOrder) {
                    auto& q = core.queues[static_cast<std::size_t>(p)];
                    if (!q.empty()) {
                        job = std::move(q.front());
                        q.pop_front();
                        found = true;
                        break;
                    }
                }
                if (found) {
                    break;
                }
                if (shutdown_flag_.load(std::memory_order_acquire)) {
                    return;
                }
                core.cv.wait(lock);
            }
        }
        job();
    }
}

void ThreadPerCoreScheduler::submit(Job job, const SubmitOptions& opts) {
    std::size_t target = opts.core_hint.has_value() ? (*opts.core_hint % num_cores_)
                                                      : (next_.fetch_add(1, std::memory_order_relaxed) % num_cores_);

    metrics_.record_submitted();
    CoreQueue& core = *cores_[target];
    {
        std::lock_guard<std::mutex> lock(core.mu);
        core.queues[static_cast<std::size_t>(opts.priority)].push_back(std::move(job));
    }
    core.cv.notify_one();
}

void ThreadPerCoreScheduler::shutdown() {
    shutdown_flag_.store(true, std::memory_order_seq_cst);
    for (auto& core : cores_) {
        core->cv.notify_all();
    }
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

RuntimeMetrics ThreadPerCoreScheduler::metrics() const { return metrics_.snapshot(); }

std::size_t ThreadPerCoreScheduler::worker_count() const { return num_cores_; }

void ThreadPerCoreScheduler::record_panic() noexcept { metrics_.record_panicked(); }

void ThreadPerCoreScheduler::record_completed() noexcept { metrics_.record_completed(); }

} // namespace wss
