#include <cstdint>
#include <cstdio>

#include "wss/scheduler.hpp"
#include "wss/work_stealing_scheduler.hpp"

int main() {
    wss::WorkStealingScheduler scheduler(4);

    auto handle = wss::spawn(scheduler, [] {
        std::uint64_t sum = 0;
        for (std::uint64_t i = 0; i < 1'000'000; ++i) {
            sum += i;
        }
        return sum;
    });

    auto result = handle.join();
    std::printf("result = %llu\n", static_cast<unsigned long long>(result.value()));

    scheduler.shutdown();
}
