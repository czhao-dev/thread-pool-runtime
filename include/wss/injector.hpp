#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

#include "wss/chase_lev_deque.hpp"
#include "wss/job.hpp"

namespace wss {

// A mutex-guarded MPMC queue used as the global "injector" external callers
// submit into. Deliberately simple, not lock-free — this mirrors
// GlobalQueueScheduler's own design philosophy and avoids reimplementing a
// segmented lock-free MPMC structure for what is, by design, the cold path:
// workers only reach the injector once their own local deque and every
// peer's local deque have come up empty.
class Injector {
public:
    void push(Job job);

    // Pops one job for the caller and moves up to `max_batch - 1` further
    // jobs directly into `dest` — batching amortizes the lock over several
    // tasks and gives the destination cache-friendly, contention-free
    // access to that work on subsequent local pops.
    std::optional<Job> pop_batch_into(ChaseLevDeque<Job>& dest, std::size_t max_batch);

private:
    std::mutex mu_;
    std::deque<Job> queue_;
};

} // namespace wss
