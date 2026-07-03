#pragma once

#include <atomic>
#include <cstdint>
#include <ostream>

namespace wss {

// A point-in-time snapshot of scheduler activity counters. Shared shape
// across all four Scheduler backends so they can be compared uniformly.
struct RuntimeMetrics {
    std::uint64_t tasks_submitted = 0;
    std::uint64_t tasks_completed = 0;
    std::uint64_t tasks_panicked = 0;
    // Cross-queue task movement. Always 0 for backends with no such concept
    // (GlobalQueueScheduler, FairScheduler) and structurally always 0 for
    // ThreadPerCoreScheduler (no code path ever moves a task between cores).
    std::uint64_t tasks_stolen = 0;

    bool operator==(const RuntimeMetrics&) const = default;
};

std::ostream& operator<<(std::ostream& os, const RuntimeMetrics& m);

class Metrics {
public:
    void record_submitted() noexcept { submitted_.fetch_add(1, std::memory_order_relaxed); }
    void record_completed() noexcept { completed_.fetch_add(1, std::memory_order_relaxed); }
    void record_panicked() noexcept { panicked_.fetch_add(1, std::memory_order_relaxed); }
    void record_stolen() noexcept { stolen_.fetch_add(1, std::memory_order_relaxed); }

    RuntimeMetrics snapshot() const noexcept {
        return RuntimeMetrics{
            submitted_.load(std::memory_order_relaxed),
            completed_.load(std::memory_order_relaxed),
            panicked_.load(std::memory_order_relaxed),
            stolen_.load(std::memory_order_relaxed),
        };
    }

private:
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> panicked_{0};
    std::atomic<std::uint64_t> stolen_{0};
};

} // namespace wss
