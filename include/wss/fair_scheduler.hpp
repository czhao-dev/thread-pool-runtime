#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "wss/idle_signal.hpp"
#include "wss/job.hpp"
#include "wss/metrics.hpp"
#include "wss/scheduler.hpp"

namespace wss {

// A CFS-inspired virtual-runtime (vruntime) fair scheduler over named
// classes with weights, using std::multimap<double, ClassId> as the
// dispatch-ordering structure (smallest vruntime first).
//
// Unlike the Linux kernel's CFS, task bodies here are opaque, non-
// preemptible closures — there's no way to interrupt one mid-execution the
// way the kernel preempts a running thread. So vruntime only ever advances
// *after* a task completes: vruntime += elapsed_wall_seconds / weight,
// measured with steady_clock.
//
// Dispatch model — bounded staleness, multi-worker: at most one live
// std::multimap entry exists per class at a time, representing "this
// class's next dispatchable task, at its last-known vruntime." As soon as a
// worker dequeues a task from a class, that class is immediately
// re-enqueued (if more work remains) at its *current* vruntime, before the
// dequeued task has even run. This lets multiple idle workers pick up the
// same class concurrently (good utilization) at the cost of a small,
// self-correcting staleness: an entry sitting in the tree can be up to
// num_workers tasks "behind" the class's true current vruntime, since
// sibling in-flight tasks from the same class haven't completed and
// updated it yet. This converges every time that entry is itself
// dispatched (which always uses the freshest vruntime value at that
// moment) — not retroactively "fixed" mid-flight, since doing so would
// require tracking and erasing specific multimap iterators from concurrent
// contexts for a marginal precision gain not justified at this scope.
//
// A newly registered (or previously-drained, now-reawakened) class has its
// vruntime clamped up to the scheduler's tracked min_vruntime_ floor before
// it can be dispatched — mirroring real CFS's "place near min_vruntime"
// rule. This prevents a class starting at vruntime 0 from instantly
// dominating a pool where other classes have already advanced, and
// prevents a class that's been idle a while from being unfairly starved by
// its own stale, comparatively-low vruntime.
class FairScheduler final : public Scheduler {
public:
    using ClassId = std::size_t;

    explicit FairScheduler(std::size_t num_workers);
    ~FairScheduler() override;

    FairScheduler(const FairScheduler&) = delete;
    FairScheduler& operator=(const FairScheduler&) = delete;

    // Pre-registers a class with an explicit weight. Optional — submit()
    // will implicitly create a class on first use via opts.class_name /
    // opts.weight. Throws std::invalid_argument if weight <= 0 or the name
    // is already registered.
    void register_class(const std::string& name, double weight);

    // Reads opts.class_name ("" maps to a class named "default") and
    // opts.weight (used only the first time this class is seen — either
    // via an earlier register_class() call or an earlier submit()).
    void submit(Job job, const SubmitOptions& opts) override;

    void shutdown() override;
    RuntimeMetrics metrics() const override;
    std::size_t worker_count() const override;
    void record_panic() noexcept override;

    struct ClassSnapshot {
        std::string name;
        double weight;
        double vruntime;
        std::uint64_t completed;
        double total_busy_seconds;
    };

    // A point-in-time view of every registered class's fairness
    // accounting. Not part of the Scheduler interface (per-class, not
    // scalar) — for tests and the benchmark suite's fairness workload.
    std::vector<ClassSnapshot> fairness_snapshot() const;

private:
    struct ClassState {
        std::string name;
        double weight = 1.0;
        double vruntime = 0.0;
        std::deque<Job> runnable;
        bool has_entry = false;
        std::uint64_t completed = 0;
        double total_busy_seconds = 0.0;

        // Explicit: libstdc++'s deque copy constructor isn't SFINAE-friendly, so
        // is_copy_constructible<ClassState> reports true despite the move-only
        // Job in `runnable`; that plus deque's move ctor not being noexcept in
        // libstdc++ makes vector<ClassState> reallocation pick the (broken) copy
        // path under GCC. Deleting copy and forcing move noexcept fixes it.
        ClassState() = default;
        ClassState(const ClassState&) = delete;
        ClassState& operator=(const ClassState&) = delete;
        ClassState(ClassState&&) noexcept = default;
        ClassState& operator=(ClassState&&) noexcept = default;
    };

    ClassId find_or_create_class_locked(const std::string& name, double weight);
    Job wrap_with_bookkeeping(Job job);
    void worker_loop();

    mutable std::mutex mu_;
    std::unordered_map<std::string, ClassId> name_to_id_;
    std::vector<ClassState> classes_;
    std::multimap<double, ClassId> ready_;
    double min_vruntime_ = 0.0;

    IdleSignal idle_;
    std::atomic<std::size_t> pending_{0};
    std::atomic<bool> shutdown_flag_{false};
    Metrics metrics_;
    std::vector<std::jthread> workers_;
    std::size_t num_workers_;
};

} // namespace wss
