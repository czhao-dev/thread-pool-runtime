// Compares all four Scheduler backends against two simpler baselines
// (single_threaded, thread_per_task) across three CPU-bound workloads, plus
// a dedicated fairness workload that's the actual reason FairScheduler
// exists. See the README for a summary of results and what they imply
// about each backend's tradeoffs.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "wss/fair_scheduler.hpp"
#include "wss/global_queue_scheduler.hpp"
#include "wss/scheduler.hpp"
#include "wss/thread_per_core_scheduler.hpp"
#include "wss/work_stealing_scheduler.hpp"

namespace {

// Deterministic, allocation-free busy-work standing in for a real CPU-bound
// task body. `n` controls how long a single task takes to run; `seed`
// (typically the task index) must vary across calls so the optimizer can't
// prove repeated calls are redundant and fold the whole workload into a
// single constant, which would make the single-threaded baseline measure
// nothing at all.
std::uint64_t cpu_work(std::uint64_t seed, std::uint64_t n) {
    std::uint64_t acc = seed;
    for (std::uint64_t i = 0; i < n; ++i) {
        acc = (acc ^ (i * 2654435761ull)) * 0x9E3779B97F4A7C15ull;
        acc = (acc << 13) | (acc >> (64 - 13));
    }
    return acc;
}

constexpr std::uint64_t kSmallWork = 2000;
constexpr std::uint64_t kLargeWork = 2'000'000;

unsigned worker_count() {
    unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? n : 8;
}

std::uint64_t small_work(int) { return kSmallWork; }
std::uint64_t large_work(int) { return kLargeWork; }
// One task in ten is ~100x heavier than the rest, the kind of skew that
// makes a single shared queue and a work-stealing pool diverge.
std::uint64_t uneven_work(int i) { return (i % 10 == 0) ? kLargeWork : kSmallWork; }

struct Workload {
    const char* name;
    int num_tasks;
    std::uint64_t (*work_for)(int);
};

constexpr Workload kWorkloads[] = {
    {"many_small_tasks", 2000, small_work},
    {"fewer_large_tasks", 64, large_work},
    {"uneven_durations", 500, uneven_work},
};

void single_threaded(int num_tasks, std::uint64_t (*work_for)(int)) {
    std::uint64_t acc = 0;
    for (int i = 0; i < num_tasks; ++i) {
        acc += cpu_work(static_cast<std::uint64_t>(i), work_for(i));
    }
    benchmark::DoNotOptimize(acc);
}

void thread_per_task(int num_tasks, std::uint64_t (*work_for)(int)) {
    std::vector<std::thread> threads;
    std::vector<std::uint64_t> results(static_cast<std::size_t>(num_tasks));
    threads.reserve(static_cast<std::size_t>(num_tasks));
    for (int i = 0; i < num_tasks; ++i) {
        std::uint64_t work = work_for(i);
        threads.emplace_back(
            [i, work, &results] { results[static_cast<std::size_t>(i)] = cpu_work(static_cast<std::uint64_t>(i), work); });
    }
    for (auto& t : threads) {
        t.join();
    }
    benchmark::DoNotOptimize(results);
}

void via_scheduler(wss::Scheduler& scheduler, int num_tasks, std::uint64_t (*work_for)(int)) {
    std::vector<wss::JoinHandle<std::uint64_t>> handles;
    handles.reserve(static_cast<std::size_t>(num_tasks));
    for (int i = 0; i < num_tasks; ++i) {
        std::uint64_t work = work_for(i);
        handles.push_back(
            wss::spawn(scheduler, [i, work] { return cpu_work(static_cast<std::uint64_t>(i), work); }));
    }
    for (auto& h : handles) {
        std::uint64_t value = h.join().value();
        benchmark::DoNotOptimize(value);
    }
}

void BM_SingleThreaded(benchmark::State& state, Workload w) {
    for (auto _ : state) {
        single_threaded(w.num_tasks, w.work_for);
    }
}

void BM_ThreadPerTask(benchmark::State& state, Workload w) {
    for (auto _ : state) {
        thread_per_task(w.num_tasks, w.work_for);
    }
}

void BM_GlobalQueue(benchmark::State& state, Workload w) {
    wss::GlobalQueueScheduler scheduler(worker_count());
    for (auto _ : state) {
        via_scheduler(scheduler, w.num_tasks, w.work_for);
    }
    scheduler.shutdown();
}

void BM_WorkStealing(benchmark::State& state, Workload w) {
    wss::WorkStealingScheduler scheduler(worker_count());
    for (auto _ : state) {
        via_scheduler(scheduler, w.num_tasks, w.work_for);
    }
    scheduler.shutdown();
}

void BM_ThreadPerCore(benchmark::State& state, Workload w) {
    wss::ThreadPerCoreScheduler scheduler(worker_count());
    for (auto _ : state) {
        via_scheduler(scheduler, w.num_tasks, w.work_for);
    }
    scheduler.shutdown();
}

void BM_Fair(benchmark::State& state, Workload w) {
    wss::FairScheduler scheduler(worker_count());
    for (auto _ : state) {
        via_scheduler(scheduler, w.num_tasks, w.work_for);
    }
    scheduler.shutdown();
}

void register_workload_benchmarks() {
    for (const Workload& w : kWorkloads) {
        std::string prefix = w.name;
        benchmark::RegisterBenchmark((prefix + "/single_threaded").c_str(), BM_SingleThreaded, w)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark((prefix + "/thread_per_task").c_str(), BM_ThreadPerTask, w)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark((prefix + "/global_queue").c_str(), BM_GlobalQueue, w)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark((prefix + "/work_stealing").c_str(), BM_WorkStealing, w)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark((prefix + "/thread_per_core").c_str(), BM_ThreadPerCore, w)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark((prefix + "/fair_scheduler").c_str(), BM_Fair, w)->Unit(benchmark::kMillisecond);
    }
}

// The fairness workload: two classes ("heavy", weight 4; "light", weight 1)
// each backlogged with an equal number of tasks. We measure completed-count
// share while BOTH classes are still backlogged (poll until half the total
// work is done) rather than after everything finishes -- waiting for full
// drain would show equal totals regardless of weight, since every
// submitted task eventually runs; the weight governs *when* work gets
// done, not *whether*. Run identically against GlobalQueueScheduler and
// WorkStealingScheduler (which have no class-fairness concept at all, so
// "heavy"/"light" here are just external bookkeeping labels, not something
// either backend reads) as a contrast baseline -- this comparison is
// FairScheduler's actual justification, not an afterthought.
struct ClassCounters {
    std::atomic<std::uint64_t> heavy{0};
    std::atomic<std::uint64_t> light{0};
};

void run_fairness_workload(benchmark::State& state, wss::Scheduler& scheduler, bool tag_classes) {
    constexpr int kTasksPerClass = 2000;
    constexpr auto kCost = std::chrono::microseconds(100);

    ClassCounters counters;
    std::vector<wss::JoinHandle<void>> handles;
    handles.reserve(static_cast<std::size_t>(kTasksPerClass) * 2);

    for (int i = 0; i < kTasksPerClass; ++i) {
        wss::SubmitOptions heavy_opts;
        if (tag_classes) {
            heavy_opts.class_name = "heavy";
            heavy_opts.weight = 4.0;
        }
        handles.push_back(wss::spawn(
            scheduler,
            [kCost, &counters] {
                std::this_thread::sleep_for(kCost);
                counters.heavy.fetch_add(1, std::memory_order_relaxed);
            },
            heavy_opts));

        wss::SubmitOptions light_opts;
        if (tag_classes) {
            light_opts.class_name = "light";
            light_opts.weight = 1.0;
        }
        handles.push_back(wss::spawn(
            scheduler,
            [kCost, &counters] {
                std::this_thread::sleep_for(kCost);
                counters.light.fetch_add(1, std::memory_order_relaxed);
            },
            light_opts));
    }

    const std::uint64_t target = kTasksPerClass;
    for (;;) {
        std::uint64_t total =
            counters.heavy.load(std::memory_order_relaxed) + counters.light.load(std::memory_order_relaxed);
        if (total >= target) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto heavy_completed = static_cast<double>(counters.heavy.load(std::memory_order_relaxed));
    auto light_completed = static_cast<double>(counters.light.load(std::memory_order_relaxed));
    state.counters["heavy_completed"] = heavy_completed;
    state.counters["light_completed"] = light_completed;
    state.counters["ratio"] = light_completed > 0 ? heavy_completed / light_completed : 0.0;

    for (auto& h : handles) {
        h.join();
    }
}

void BM_FairnessUnderClassSkew_FairScheduler(benchmark::State& state) {
    for (auto _ : state) {
        wss::FairScheduler scheduler(2);
        run_fairness_workload(state, scheduler, /*tag_classes=*/true);
        scheduler.shutdown();
    }
}

void BM_FairnessUnderClassSkew_GlobalQueue(benchmark::State& state) {
    for (auto _ : state) {
        wss::GlobalQueueScheduler scheduler(2);
        run_fairness_workload(state, scheduler, /*tag_classes=*/false);
        scheduler.shutdown();
    }
}

void BM_FairnessUnderClassSkew_WorkStealing(benchmark::State& state) {
    for (auto _ : state) {
        wss::WorkStealingScheduler scheduler(2);
        run_fairness_workload(state, scheduler, /*tag_classes=*/false);
        scheduler.shutdown();
    }
}

} // namespace

int main(int argc, char** argv) {
    register_workload_benchmarks();
    benchmark::RegisterBenchmark("fairness_under_class_skew/fair_scheduler", BM_FairnessUnderClassSkew_FairScheduler);
    benchmark::RegisterBenchmark("fairness_under_class_skew/global_queue", BM_FairnessUnderClassSkew_GlobalQueue);
    benchmark::RegisterBenchmark("fairness_under_class_skew/work_stealing", BM_FairnessUnderClassSkew_WorkStealing);

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
