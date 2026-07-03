#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "wss/fair_scheduler.hpp"
#include "wss/scheduler.hpp"

// Two tenants share a pool: "batch" is weighted 4x "interactive". Both
// submit a large, equal-sized backlog of small tasks. While both queues are
// still backlogged, batch gets dispatched roughly 4x as often as
// interactive -- that's the proportional-share property this scheduler
// exists to provide. If we simply waited for every task to finish, both
// classes would end up with equal completed counts regardless of weight
// (everything submitted eventually runs); the weight's effect shows up in
// *when* each class's work gets done, not *whether* it does. So this
// example prints a snapshot mid-flight (while backlog remains) to make
// that share visible, then a final snapshot once everything has drained.
int main() {
    wss::FairScheduler scheduler(2);
    scheduler.register_class("batch", 4.0);
    scheduler.register_class("interactive", 1.0);

    constexpr int kTasksPerClass = 3000;
    constexpr auto kTaskCost = std::chrono::microseconds(100);

    std::vector<wss::JoinHandle<void>> handles;
    for (int i = 0; i < kTasksPerClass; ++i) {
        wss::SubmitOptions batch_opts;
        batch_opts.class_name = "batch";
        handles.push_back(wss::spawn(scheduler, [kTaskCost] { std::this_thread::sleep_for(kTaskCost); }, batch_opts));

        wss::SubmitOptions interactive_opts;
        interactive_opts.class_name = "interactive";
        handles.push_back(
            wss::spawn(scheduler, [kTaskCost] { std::this_thread::sleep_for(kTaskCost); }, interactive_opts));
    }

    // Wait until roughly half the total backlog has drained, then look --
    // both classes are still backlogged at this point, so the completed
    // counts reflect the scheduler's proportional-share decisions, not just
    // "everything eventually finishes."
    const std::uint64_t target = kTasksPerClass;
    for (;;) {
        std::uint64_t total = 0;
        for (const auto& c : scheduler.fairness_snapshot()) {
            total += c.completed;
        }
        if (total >= target) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::printf("-- mid-flight (both classes still backlogged) --\n");
    for (const auto& c : scheduler.fairness_snapshot()) {
        std::printf("class=%-12s weight=%.1f completed=%-6llu vruntime=%.6f\n", c.name.c_str(), c.weight,
                    static_cast<unsigned long long>(c.completed), c.vruntime);
    }

    for (auto& h : handles) {
        h.join();
    }

    std::printf("-- final (fully drained; both classes converge to equal totals) --\n");
    for (const auto& c : scheduler.fairness_snapshot()) {
        std::printf("class=%-12s weight=%.1f completed=%-6llu vruntime=%.6f\n", c.name.c_str(), c.weight,
                    static_cast<unsigned long long>(c.completed), c.vruntime);
    }

    scheduler.shutdown();
}
