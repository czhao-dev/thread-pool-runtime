//! Compares the work-stealing `Runtime` against three simpler execution
//! strategies across a few representative workloads. See the README for a
//! summary of results and what they imply about each strategy's tradeoffs.

use std::hint::black_box;
use std::time::Duration;

use criterion::{criterion_group, criterion_main, Criterion};
use work_stealing_thread_pool::{run_graph, GlobalQueuePool, Runtime, TaskGraph};

/// Deterministic, allocation-free busy-work standing in for a real CPU-bound
/// task body. `n` controls how long a single task takes to run; `seed`
/// (typically the task index) must vary across calls so the optimizer can't
/// prove that repeated calls are redundant and fold the whole workload into
/// a single constant, which would make the single-threaded baseline
/// measure nothing at all.
fn cpu_work(seed: u64, n: u64) -> u64 {
    let mut acc: u64 = seed;
    for i in 0..n {
        acc = (acc ^ i.wrapping_mul(2654435761))
            .wrapping_mul(0x9E3779B97F4A7C15)
            .rotate_left(13);
    }
    acc
}

const SMALL_WORK: u64 = 2_000;
const LARGE_WORK: u64 = 2_000_000;

fn single_threaded(num_tasks: usize, work_for: impl Fn(usize) -> u64) {
    let mut acc = 0u64;
    for i in 0..num_tasks {
        acc = acc.wrapping_add(cpu_work(i as u64, work_for(i)));
    }
    black_box(acc);
}

fn thread_per_task(num_tasks: usize, work_for: impl Fn(usize) -> u64) {
    let handles: Vec<_> = (0..num_tasks)
        .map(|i| {
            let work = work_for(i);
            std::thread::spawn(move || cpu_work(i as u64, work))
        })
        .collect();
    for handle in handles {
        black_box(handle.join().unwrap());
    }
}

fn global_queue_pool(pool: &GlobalQueuePool, num_tasks: usize, work_for: impl Fn(usize) -> u64) {
    let handles: Vec<_> = (0..num_tasks)
        .map(|i| {
            let work = work_for(i);
            pool.spawn(move || cpu_work(i as u64, work))
        })
        .collect();
    for handle in handles {
        black_box(handle.join().unwrap());
    }
}

fn work_stealing_pool(runtime: &Runtime, num_tasks: usize, work_for: impl Fn(usize) -> u64) {
    let handles: Vec<_> = (0..num_tasks)
        .map(|i| {
            let work = work_for(i);
            runtime.spawn(move || cpu_work(i as u64, work))
        })
        .collect();
    for handle in handles {
        black_box(handle.join().unwrap());
    }
}

/// Runs the four-way comparison for one workload, where `work_for(i)` gives
/// the amount of busy-work task `i` should perform.
fn bench_workload(
    c: &mut Criterion,
    name: &str,
    num_tasks: usize,
    work_for: impl Fn(usize) -> u64 + Copy,
) {
    let workers = num_cpus::get();
    let global_pool = GlobalQueuePool::new(workers);
    let work_stealing = Runtime::new(workers);

    let mut group = c.benchmark_group(name);
    group.sample_size(20);
    group.measurement_time(Duration::from_secs(4));

    group.bench_function("single_threaded", |b| {
        b.iter(|| single_threaded(num_tasks, work_for))
    });
    group.bench_function("thread_per_task", |b| {
        b.iter(|| thread_per_task(num_tasks, work_for))
    });
    group.bench_function("global_queue_pool", |b| {
        b.iter(|| global_queue_pool(&global_pool, num_tasks, work_for))
    });
    group.bench_function("work_stealing_pool", |b| {
        b.iter(|| work_stealing_pool(&work_stealing, num_tasks, work_for))
    });

    group.finish();
    global_pool.shutdown();
    work_stealing.shutdown();
}

fn many_small_tasks(c: &mut Criterion) {
    bench_workload(c, "many_small_tasks", 2_000, |_| SMALL_WORK);
}

fn fewer_large_tasks(c: &mut Criterion) {
    bench_workload(c, "fewer_large_tasks", 64, |_| LARGE_WORK);
}

fn uneven_task_durations(c: &mut Criterion) {
    // One task in ten is ~100x heavier than the rest, the kind of skew that
    // makes a single shared queue and a work-stealing pool diverge.
    bench_workload(c, "uneven_durations", 500, |i| {
        if i % 10 == 0 {
            LARGE_WORK
        } else {
            SMALL_WORK
        }
    });
}

/// Fork-join: a fan-out of independent leaves followed by a join task that
/// depends on all of them, scheduled through `TaskGraph`/`run_graph`. There
/// is no equivalent baseline for the simpler strategies since none of them
/// understand dependencies, so this only measures the work-stealing pool.
fn dependency_graph_fan_out(c: &mut Criterion) {
    let workers = num_cpus::get();
    let runtime = Runtime::new(workers);

    let mut group = c.benchmark_group("dependency_graph");
    group.sample_size(20);
    group.measurement_time(Duration::from_secs(4));

    group.bench_function("fan_out_200_then_join", |b| {
        b.iter(|| {
            let mut graph = TaskGraph::new();
            let leaves: Vec<_> = (0..200u64)
                .map(|seed| graph.add_task(move || cpu_work(seed, SMALL_WORK)))
                .collect();
            let join = graph.add_task_after(leaves.clone(), move |results| {
                leaves
                    .iter()
                    .map(|&id| results.get::<u64>(id))
                    .fold(0u64, u64::wrapping_add)
            });
            let results = run_graph(&runtime, graph);
            black_box(results.get::<u64>(join));
        })
    });

    group.finish();
    runtime.shutdown();
}

criterion_group!(
    benches,
    many_small_tasks,
    fewer_large_tasks,
    uneven_task_durations,
    dependency_graph_fan_out
);
criterion_main!(benches);
