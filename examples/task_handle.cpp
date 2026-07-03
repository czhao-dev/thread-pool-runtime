#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "wss/scheduler.hpp"
#include "wss/work_stealing_scheduler.hpp"

int main() {
    wss::WorkStealingScheduler scheduler(4);

    auto handle = wss::spawn(scheduler, [] { return std::string("hello from worker"); });
    auto value = handle.join().value();
    assert(value == "hello from worker");
    std::printf("got: %s\n", value.c_str());

    auto panicking = wss::spawn(scheduler, []() -> std::uint32_t { throw std::runtime_error("deliberate failure"); });
    auto result = panicking.join();
    if (!result.is_ok()) {
        std::printf("task failed as expected: %s\n", result.error().message.c_str());
    }

    scheduler.shutdown();
}
