#include <cstdio>
#include <thread>
#include <vector>

#include "wss/scheduler.hpp"
#include "wss/thread_per_core_scheduler.hpp"

int main() {
    wss::ThreadPerCoreScheduler scheduler(4);

    // Pin two tasks to the same core explicitly and confirm they both
    // report the identical worker thread id — the "never migrates" promise
    // made concrete.
    wss::SubmitOptions opts;
    opts.core_hint = 0;

    auto first = wss::spawn(scheduler, [] { return std::this_thread::get_id(); }, opts);
    auto id_a = first.join().value();

    auto second = wss::spawn(scheduler, [] { return std::this_thread::get_id(); }, opts);
    auto id_b = second.join().value();

    std::printf("both tasks pinned to core 0 ran on the %s worker thread\n", id_a == id_b ? "same" : "DIFFERENT");

    // Default (round-robin) placement spreads work across every core.
    std::vector<wss::JoinHandle<std::thread::id>> handles;
    for (int i = 0; i < 8; ++i) {
        handles.push_back(wss::spawn(scheduler, [] { return std::this_thread::get_id(); }));
    }
    for (auto& h : handles) {
        h.join();
    }

    auto metrics = scheduler.metrics();
    std::printf("submitted=%llu completed=%llu stolen=%llu (structurally always 0)\n",
                static_cast<unsigned long long>(metrics.tasks_submitted),
                static_cast<unsigned long long>(metrics.tasks_completed),
                static_cast<unsigned long long>(metrics.tasks_stolen));

    scheduler.shutdown();
}
