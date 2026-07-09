#include "wss/fair_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>

namespace wss {

namespace {
constexpr const char* kDefaultClassName = "default";
} // namespace

FairScheduler::FairScheduler(std::size_t num_workers) : num_workers_(num_workers) {
    if (num_workers == 0) {
        throw std::invalid_argument("a scheduler needs at least one worker thread");
    }

    workers_.reserve(num_workers);
    for (std::size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

FairScheduler::~FairScheduler() { shutdown(); }

FairScheduler::ClassId FairScheduler::find_or_create_class_locked(const std::string& name, double weight) {
    auto it = name_to_id_.find(name);
    if (it != name_to_id_.end()) {
        return it->second;
    }
    if (weight <= 0.0) {
        throw std::invalid_argument("class weight must be positive");
    }

    ClassId id = classes_.size();
    ClassState state;
    state.name = name;
    state.weight = weight;
    state.vruntime = min_vruntime_;
    classes_.push_back(std::move(state));
    name_to_id_.emplace(name, id);
    return id;
}

void FairScheduler::register_class(const std::string& name, double weight) {
    std::lock_guard<std::mutex> lock(mu_);
    if (name_to_id_.contains(name)) {
        throw std::invalid_argument("class '" + name + "' is already registered");
    }
    find_or_create_class_locked(name, weight);
}

Job FairScheduler::wrap_with_bookkeeping(Job job) {
    return Job([this, job = std::move(job)]() mutable {
        job();
        pending_.fetch_sub(1, std::memory_order_seq_cst);
        idle_.notify();
    });
}

void FairScheduler::submit(Job job, const SubmitOptions& opts) {
    metrics_.record_submitted();
    pending_.fetch_add(1, std::memory_order_seq_cst);

    const std::string& class_name = opts.class_name.empty() ? std::string(kDefaultClassName) : opts.class_name;
    double weight = opts.weight > 0.0 ? opts.weight : 1.0;
    Job wrapped = wrap_with_bookkeeping(std::move(job));

    {
        std::lock_guard<std::mutex> lock(mu_);
        ClassId id = find_or_create_class_locked(class_name, weight);
        ClassState& state = classes_[id];
        state.runnable.push_back(std::move(wrapped));
        if (!state.has_entry) {
            state.vruntime = std::max(state.vruntime, min_vruntime_);
            ready_.insert({state.vruntime, id});
            state.has_entry = true;
        }
    }
    idle_.notify();
}

void FairScheduler::worker_loop() {
    for (;;) {
        auto gen = idle_.current();

        std::optional<std::pair<ClassId, Job>> dispatched;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!ready_.empty()) {
                auto it = ready_.begin();
                ClassId id = it->second;
                double dispatch_vruntime = it->first;
                ready_.erase(it);
                min_vruntime_ = std::max(min_vruntime_, dispatch_vruntime);

                ClassState& state = classes_[id];
                Job job = std::move(state.runnable.front());
                state.runnable.pop_front();
                if (!state.runnable.empty()) {
                    ready_.insert({state.vruntime, id});
                } else {
                    state.has_entry = false;
                }
                dispatched.emplace(id, std::move(job));
            }
        }

        if (dispatched) {
            auto start = std::chrono::steady_clock::now();
            dispatched->second();
            double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

            std::lock_guard<std::mutex> lock(mu_);
            ClassState& state = classes_[dispatched->first];
            state.vruntime += elapsed / state.weight;
            state.completed += 1;
            state.total_busy_seconds += elapsed;
            continue;
        }

        if (shutdown_flag_.load(std::memory_order_acquire) && pending_.load(std::memory_order_acquire) == 0) {
            break;
        }
        idle_.wait(gen);
    }
}

void FairScheduler::shutdown() {
    shutdown_flag_.store(true, std::memory_order_seq_cst);
    idle_.notify();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

RuntimeMetrics FairScheduler::metrics() const { return metrics_.snapshot(); }

std::size_t FairScheduler::worker_count() const { return num_workers_; }

void FairScheduler::record_panic() noexcept { metrics_.record_panicked(); }

void FairScheduler::record_completed() noexcept { metrics_.record_completed(); }

std::vector<FairScheduler::ClassSnapshot> FairScheduler::fairness_snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ClassSnapshot> result;
    result.reserve(classes_.size());
    for (const auto& c : classes_) {
        result.push_back(ClassSnapshot{c.name, c.weight, c.vruntime, c.completed, c.total_busy_seconds});
    }
    return result;
}

} // namespace wss
