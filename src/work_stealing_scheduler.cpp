#include "wss/work_stealing_scheduler.hpp"

#include <optional>
#include <stdexcept>
#include <utility>

namespace wss {

namespace {
constexpr std::size_t kInjectorBatchSize = 32;
} // namespace

std::optional<std::pair<Job, WorkStealingScheduler::FoundKind>> WorkStealingScheduler::find_task(
    std::size_t worker_id, Shared& shared) {
    auto& own = shared.local_deques[worker_id];

    for (Priority p : kPriorityOrder) {
        if (auto job = own[static_cast<std::size_t>(p)]->pop()) {
            return std::make_pair(std::move(*job), FoundKind::kLocal);
        }
    }

    for (Priority p : kPriorityOrder) {
        auto idx = static_cast<std::size_t>(p);
        if (auto job = shared.injectors[idx].pop_batch_into(*own[idx], kInjectorBatchSize)) {
            return std::make_pair(std::move(*job), FoundKind::kLocal);
        }
    }

    for (Priority p : kPriorityOrder) {
        auto idx = static_cast<std::size_t>(p);
        for (std::size_t peer = 0; peer < shared.local_deques.size(); ++peer) {
            if (peer == worker_id) {
                continue;
            }
            if (auto job = shared.local_deques[peer][idx]->steal()) {
                return std::make_pair(std::move(*job), FoundKind::kStolen);
            }
        }
    }

    return std::nullopt;
}

WorkStealingScheduler::WorkStealingScheduler(std::size_t num_workers) : num_workers_(num_workers) {
    if (num_workers == 0) {
        throw std::invalid_argument("a scheduler needs at least one worker thread");
    }

    shared_ = std::make_shared<Shared>();
    shared_->local_deques.resize(num_workers);
    for (auto& per_priority : shared_->local_deques) {
        for (auto& deque : per_priority) {
            deque = std::make_unique<ChaseLevDeque<Job>>();
        }
    }

    workers_.reserve(num_workers);
    for (std::size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back([shared = shared_, i] { run_worker(i, shared); });
    }
}

WorkStealingScheduler::~WorkStealingScheduler() { shutdown(); }

void WorkStealingScheduler::run_worker(std::size_t id, std::shared_ptr<Shared> shared) {
    for (;;) {
        auto gen = shared->idle.current();
        if (auto found = find_task(id, *shared)) {
            auto& [job, kind] = *found;
            if (kind == FoundKind::kStolen) {
                shared->metrics.record_stolen();
            }
            job();
            continue;
        }
        if (shared->shutdown_flag.load(std::memory_order_acquire) &&
            shared->pending.load(std::memory_order_acquire) == 0) {
            break;
        }
        shared->idle.wait(gen);
    }
}

void WorkStealingScheduler::submit(Job job, const SubmitOptions& opts) {
    shared_->metrics.record_submitted();
    shared_->pending.fetch_add(1, std::memory_order_seq_cst);

    auto shared = shared_;
    Job wrapped([shared, job = std::move(job)]() mutable {
        job();
        shared->pending.fetch_sub(1, std::memory_order_seq_cst);
        shared->metrics.record_completed();
        shared->idle.notify();
    });

    shared_->injectors[static_cast<std::size_t>(opts.priority)].push(std::move(wrapped));
    shared_->idle.notify();
}

void WorkStealingScheduler::shutdown() {
    shared_->shutdown_flag.store(true, std::memory_order_seq_cst);
    shared_->idle.notify();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

RuntimeMetrics WorkStealingScheduler::metrics() const { return shared_->metrics.snapshot(); }

std::size_t WorkStealingScheduler::worker_count() const { return num_workers_; }

} // namespace wss
