#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "wss/chase_lev_deque.hpp"
#include "wss/idle_signal.hpp"
#include "wss/injector.hpp"
#include "wss/job.hpp"
#include "wss/metrics.hpp"
#include "wss/priority.hpp"
#include "wss/scheduler.hpp"

namespace wss {

// A fixed-size pool of worker threads that execute submitted tasks using
// per-priority work-stealing queues. Each worker searches, in priority
// order at every stage: its own local deques (High -> Normal -> Background),
// then the global per-priority injectors (a batch pull that also seeds the
// worker's local deque), then every peer worker's local deque.
class WorkStealingScheduler final : public Scheduler {
public:
    explicit WorkStealingScheduler(std::size_t num_workers);
    ~WorkStealingScheduler() override;

    WorkStealingScheduler(const WorkStealingScheduler&) = delete;
    WorkStealingScheduler& operator=(const WorkStealingScheduler&) = delete;

    void submit(Job job, const SubmitOptions& opts) override;

    // Idempotent: stops accepting the premise that more work is coming,
    // lets every already-submitted task run to completion, then joins all
    // worker threads. Safe to call multiple times.
    void shutdown() override;

    RuntimeMetrics metrics() const override;
    std::size_t worker_count() const override;
    void record_panic() noexcept override;

private:
    // State shared between this handle and every worker thread.
    struct Shared {
        // local_deques[worker][priority]; local_deques[w][*] is owner-only
        // for worker w's push()/pop(), but steal()-able by any peer.
        std::vector<std::array<std::unique_ptr<ChaseLevDeque<Job>>, kPriorityCount>> local_deques;
        std::array<Injector, kPriorityCount> injectors;
        std::atomic<std::size_t> pending{0};
        std::atomic<bool> shutdown_flag{false};
        IdleSignal idle;
        Metrics metrics;
    };

    enum class FoundKind { kLocal, kStolen };

    static std::optional<std::pair<Job, FoundKind>> find_task(std::size_t worker_id, Shared& shared);
    static void run_worker(std::size_t id, std::shared_ptr<Shared> shared);

    std::shared_ptr<Shared> shared_;
    std::vector<std::jthread> workers_;
    std::size_t num_workers_;
};

} // namespace wss
