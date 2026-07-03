#pragma once

#include <atomic>
#include <cstdint>

namespace wss {

// An atomic-generation doorbell used to wake idle workers promptly when new
// work arrives. Replaces a Condvar-plus-polling-timeout design entirely:
// C++20's futex-backed atomic wait()/notify() means no busy-polling and no
// bounded-wait fallback are needed, as long as callers follow the
// snapshot-before-check pattern below.
//
// Correct usage (avoids the missed-wakeup race):
//   auto gen = idle.current();      // snapshot BEFORE the final task search
//   if (auto job = find_task(...)) { run(job); continue; }
//   if (should_stop()) break;
//   idle.wait(gen);                 // no-op if gen already advanced past this
//
// If a producer calls notify() any time after the snapshot, gen_ differs
// from the observed value and wait() returns immediately instead of
// blocking — there is no window in which a wakeup can be silently missed.
class IdleSignal {
public:
    void notify() noexcept {
        gen_.fetch_add(1, std::memory_order_release);
        gen_.notify_all();
    }

    std::uint64_t current() const noexcept { return gen_.load(std::memory_order_acquire); }

    void wait(std::uint64_t observed) noexcept { gen_.wait(observed, std::memory_order_acquire); }

private:
    std::atomic<std::uint64_t> gen_{0};
};

} // namespace wss
