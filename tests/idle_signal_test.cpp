#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "wss/idle_signal.hpp"

namespace {

using namespace std::chrono_literals;

TEST(IdleSignalTest, WaitReturnsImmediatelyIfGenerationAlreadyAdvanced) {
    wss::IdleSignal idle;
    auto gen = idle.current();
    idle.notify();
    // gen no longer matches idle.current(); wait() must return right away.
    auto start = std::chrono::steady_clock::now();
    idle.wait(gen);
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 1s);
}

TEST(IdleSignalTest, NotifyWakesAWaitingThread) {
    wss::IdleSignal idle;
    std::atomic<bool> woke{false};

    auto gen = idle.current();
    std::thread waiter([&] {
        idle.wait(gen);
        woke.store(true, std::memory_order_release);
    });

    // Give the waiter a chance to actually block before notifying.
    std::this_thread::sleep_for(50ms);
    idle.notify();
    waiter.join();
    EXPECT_TRUE(woke.load(std::memory_order_acquire));
}

// The snapshot-before-check pattern this doorbell relies on: many producer
// threads racing notify() against many consumer threads racing
// current()-then-wait() must never deadlock or hang, regardless of
// interleaving. This is the property the whole no-polling design rests on.
TEST(IdleSignalTest, ManyProducersAndConsumersNeverMissAWakeup) {
    constexpr int kConsumers = 8;
    constexpr int kRounds = 2000;

    wss::IdleSignal idle;
    std::atomic<int> pending{0};
    std::atomic<bool> stop{false};
    std::atomic<int> consumed{0};

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            for (;;) {
                auto gen = idle.current();
                int expected = pending.load(std::memory_order_acquire);
                while (expected > 0) {
                    if (pending.compare_exchange_weak(expected, expected - 1, std::memory_order_acq_rel)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
                if (expected > 0) {
                    continue;
                }
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }
                idle.wait(gen);
            }
        });
    }

    std::thread producer([&] {
        for (int i = 0; i < kRounds; ++i) {
            pending.fetch_add(1, std::memory_order_release);
            idle.notify();
        }
    });
    producer.join();

    // Wait for consumers to drain everything the producer pushed.
    while (consumed.load(std::memory_order_acquire) < kRounds) {
        std::this_thread::sleep_for(1ms);
    }
    stop.store(true, std::memory_order_release);
    idle.notify();
    for (auto& t : consumers) {
        t.join();
    }

    EXPECT_EQ(consumed.load(), kRounds);
}

} // namespace
