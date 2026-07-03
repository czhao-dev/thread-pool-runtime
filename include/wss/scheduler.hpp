#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "wss/cancellation.hpp"
#include "wss/job.hpp"
#include "wss/join_handle.hpp"
#include "wss/metrics.hpp"
#include "wss/priority.hpp"
#include "wss/result.hpp"

namespace wss {

// Backend-specific submission hints. Each concrete Scheduler reads only the
// fields it cares about and ignores the rest — e.g. GlobalQueueScheduler
// ignores all of them (plain FIFO).
struct SubmitOptions {
    Priority priority = Priority::Normal;         // read by WorkStealingScheduler
    std::optional<std::size_t> core_hint;          // read by ThreadPerCoreScheduler (else round-robin)
    std::string class_name;                        // read by FairScheduler ("" = default class)
    double weight = 1.0;                             // read by FairScheduler, only on a class's first use
};

// The common interface all four scheduler backends implement, so callers
// (and the benchmark suite) can hold a std::vector<std::unique_ptr<Scheduler>>
// and drive every backend uniformly. Backend-specific extras that don't fit
// this shared shape (e.g. FairScheduler::fairness_snapshot()) live as
// additional public methods on the concrete class instead.
class Scheduler {
public:
    virtual ~Scheduler() = default;

    virtual void submit(Job job, const SubmitOptions& opts) = 0;
    virtual void shutdown() = 0;
    virtual RuntimeMetrics metrics() const = 0;
    virtual std::size_t worker_count() const = 0;
};

// Builds the Job + JoinHandle<T> machinery once, outside the virtual call,
// so no backend duplicates result-plumbing or exception-catching. This is
// the C++ analog of Rust's catch_unwind: a task body that throws has its
// exception captured and delivered through the handle as a JoinError,
// rather than propagating across the scheduling boundary.
template <class F>
auto spawn(Scheduler& s, F&& f, SubmitOptions opts = {}) -> JoinHandle<std::invoke_result_t<F>> {
    using T = std::invoke_result_t<F>;
    auto [result_setter, handle] = new_handle_pair<T>();

    Job job([f = std::forward<F>(f), setter = std::move(result_setter)]() mutable {
        try {
            if constexpr (std::is_void_v<T>) {
                f();
                setter.set(Result<T>::ok());
            } else {
                setter.set(Result<T>::ok(f()));
            }
        } catch (...) {
            setter.set(Result<T>::err(JoinError{current_exception_message(), std::current_exception()}));
        }
    });

    s.submit(std::move(job), opts);
    return std::move(handle);
}

// A cooperatively cancellable spawn: the task body receives a
// CancellationContext it can poll. Needs no Scheduler-interface changes —
// cancellation is purely a task-body concern, not a scheduling one.
template <class F>
auto spawn_cancellable(Scheduler& s, CancellationToken tok, F&& f, SubmitOptions opts = {})
    -> JoinHandle<std::invoke_result_t<F, const CancellationContext&>> {
    auto ctx = tok.context();
    return spawn(
        s,
        [ctx = std::move(ctx), f = std::forward<F>(f)]() mutable { return f(ctx); },
        std::move(opts));
}

} // namespace wss
