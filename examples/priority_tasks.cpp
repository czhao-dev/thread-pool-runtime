#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "wss/priority.hpp"
#include "wss/scheduler.hpp"
#include "wss/work_stealing_scheduler.hpp"

// Floods the pool with background work, then submits a high-priority task
// and confirms it still completes promptly by recording the order tasks
// actually ran in.
int main() {
    wss::WorkStealingScheduler scheduler(2);
    std::atomic<int> order{0};
    std::atomic<int> high_priority_rank{-1};

    std::vector<wss::JoinHandle<void>> background_handles;
    for (int i = 0; i < 200; ++i) {
        wss::SubmitOptions opts;
        opts.priority = wss::Priority::Background;
        background_handles.push_back(wss::spawn(
            scheduler,
            [&order] {
                order.fetch_add(1, std::memory_order_seq_cst);
                std::this_thread::yield();
            },
            opts));
    }

    wss::SubmitOptions high_opts;
    high_opts.priority = wss::Priority::High;
    auto high = wss::spawn(
        scheduler,
        [&order, &high_priority_rank] {
            high_priority_rank.store(order.fetch_add(1, std::memory_order_seq_cst), std::memory_order_seq_cst);
        },
        high_opts);

    high.join();
    for (auto& h : background_handles) {
        h.join();
    }

    std::printf("high-priority task finished as the #%d task to run out of 201\n",
                high_priority_rank.load(std::memory_order_seq_cst) + 1);

    scheduler.shutdown();
}
