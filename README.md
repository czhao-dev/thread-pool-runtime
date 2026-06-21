# Rust Thread Pool Runtime

A small systems-oriented task runtime written in Rust, featuring a thread pool, task handles, work stealing, cancellation, task priorities, dependency scheduling, and benchmarks.

This project is designed to explore how modern runtimes schedule work across CPU cores while preserving Rust’s safety guarantees around ownership, sharing, and concurrency.

## Overview

`rust-thread-pool-runtime` is a from-scratch task execution runtime. It starts with a basic fixed-size worker pool and gradually adds more advanced runtime features such as:

* task submission and result handles
* graceful shutdown
* work stealing between workers
* task priorities
* cancellation and timeouts
* dependency-aware task scheduling
* benchmark comparisons against simpler scheduling strategies

The goal is not to replace mature libraries such as Rayon or Tokio. Instead, this project is a learning-focused implementation of the core mechanisms behind CPU task runtimes, job schedulers, and work-stealing execution engines.

## Motivation

Thread pools are common in backend systems, build systems, compilers, storage engines, databases, and distributed services. A good runtime needs to balance several competing goals:

* keep CPU cores busy
* avoid excessive thread creation
* minimize scheduling overhead
* prevent idle workers while work exists elsewhere
* support graceful shutdown and cancellation
* safely share state across threads
* provide predictable behavior under load

Rust is a good language for this project because it forces the implementation to be explicit about ownership, lifetimes, shared state, and thread safety.

## Features

### Implemented

* [ ] Fixed-size worker thread pool
* [ ] Task submission API
* [ ] Task result handles
* [ ] Graceful shutdown
* [ ] Basic stress tests
* [ ] Benchmark harness

### Planned

* [ ] Local worker queues
* [ ] Work stealing between workers
* [ ] Task priorities
* [ ] Cancellation support
* [ ] Task timeouts
* [ ] Dependency graph scheduling
* [ ] Runtime metrics
* [ ] Comparison against baseline schedulers
* [ ] Comparison against Rayon, optional

Update this section as the implementation progresses.

## Project Goals

This project focuses on runtime and systems design rather than application-level business logic.

Main goals:

* implement a thread pool from first principles
* understand worker lifecycle management
* explore Rust concurrency primitives
* compare global-queue and work-stealing schedulers
* measure throughput and scheduling overhead
* document tradeoffs clearly

Non-goals:

* replacing Rayon or Tokio
* building a full async runtime
* supporting distributed execution
* supporting real-time scheduling guarantees
* maximizing every microbenchmark at the cost of readability

## Architecture

The runtime is organized around a small set of core components:

```text
+-------------------+
| Runtime / Pool    |
|-------------------|
| submit task       |
| return handle     |
| shutdown workers  |
+---------+---------+
          |
          v
+-------------------+
| Global Queue      |
|-------------------|
| incoming tasks    |
| fallback queue    |
+---------+---------+
          |
          v
+-------------------+        +-------------------+
| Worker 0          |        | Worker 1          |
|-------------------|        |-------------------|
| local queue       | <----> | local queue       |
| execute tasks     | steal  | execute tasks     |
+-------------------+        +-------------------+
          ^
          |
+-------------------+
| Task Handle       |
|-------------------|
| wait for result   |
| observe errors    |
| cancellation      |
+-------------------+
```

The initial implementation uses a simple shared task queue. Later milestones introduce local worker queues and work stealing to reduce contention and improve CPU utilization.

## Suggested Repository Layout

```text
rust-thread-pool-runtime/
├── Cargo.toml
├── README.md
├── benches/
│   └── scheduler_bench.rs
├── examples/
│   ├── basic_pool.rs
│   ├── task_handle.rs
│   ├── priority_tasks.rs
│   └── dependency_graph.rs
├── src/
│   ├── lib.rs
│   ├── runtime.rs
│   ├── worker.rs
│   ├── task.rs
│   ├── handle.rs
│   ├── queue.rs
│   ├── steal.rs
│   ├── priority.rs
│   ├── dependency.rs
│   ├── cancellation.rs
│   └── metrics.rs
└── tests/
    ├── basic_execution.rs
    ├── shutdown.rs
    ├── cancellation.rs
    ├── work_stealing.rs
    └── stress.rs
```

## Quick Start

Clone the repository:

```bash
git clone https://github.com/czhao-dev/rust-thread-pool-runtime.git
cd rust-thread-pool-runtime
```

Run tests:

```bash
cargo test
```

Run examples:

```bash
cargo run --example basic_pool
```

Run benchmarks:

```bash
cargo bench
```

Format and lint:

```bash
cargo fmt
cargo clippy --all-targets --all-features -- -D warnings
```

## Basic Usage

```rust
use rust_thread_pool_runtime::Runtime;

fn main() {
    let runtime = Runtime::new(4);

    let handle = runtime.spawn(|| {
        let mut sum = 0;
        for i in 0..1_000_000 {
            sum += i;
        }
        sum
    });

    let result = handle.join().unwrap();
    println!("result = {}", result);

    runtime.shutdown();
}
```

## Task Handles

Task handles allow callers to wait for task completion and retrieve results.

```rust
let handle = runtime.spawn(|| {
    "hello from worker"
});

let value = handle.join().unwrap();
assert_eq!(value, "hello from worker");
```

The implementation is intended to demonstrate how a runtime can connect task execution with result delivery using safe synchronization primitives.

## Work Stealing

The work-stealing scheduler uses per-worker local queues. Workers push newly spawned tasks to their local queue. When a worker runs out of local work, it attempts to steal tasks from other workers.

```text
Worker A local queue: [task1, task2, task3]
Worker B local queue: []

Worker B steals task1 from Worker A.
```

This reduces contention on a single global queue and improves load balancing for workloads with uneven task distribution.

## Task Priorities

Priority scheduling allows the runtime to favor latency-sensitive tasks over background work.

Example priority classes:

```text
High       interactive or latency-sensitive tasks
Normal     default tasks
Background maintenance tasks
```

Planned API:

```rust
runtime.spawn_with_priority(Priority::High, || {
    handle_request()
});

runtime.spawn_with_priority(Priority::Background, || {
    compact_logs()
});
```

## Cancellation and Timeouts

Cancellation allows a task to observe that it should stop early.

Planned API:

```rust
let token = CancellationToken::new();

let handle = runtime.spawn_cancellable(token.clone(), |ctx| {
    while !ctx.is_cancelled() {
        do_some_work();
    }
});

token.cancel();
handle.join().unwrap();
```

Cancellation is cooperative. The runtime does not forcibly kill running threads.

## Dependency Scheduling

A dependency-aware scheduler can execute a task only after its prerequisites complete.

Example:

```text
Task A ─┐
        ├──> Task C
Task B ─┘
```

Planned API:

```rust
let a = graph.add_task(|| parse_file("a.rs"));
let b = graph.add_task(|| parse_file("b.rs"));

let c = graph.add_task_after([a, b], || {
    link_outputs()
});

runtime.run_graph(graph);
```

This is useful for understanding build systems, compiler pipelines, DAG schedulers, and job orchestration systems.

## Benchmark Plan

The benchmark suite compares different execution strategies:

| Scheduler          | Description                                  |
| ------------------ | -------------------------------------------- |
| Single-threaded    | Runs all tasks sequentially                  |
| Thread-per-task    | Spawns one OS thread per task                |
| Global queue pool  | Fixed workers sharing one queue              |
| Work-stealing pool | Fixed workers with local queues and stealing |
| Rayon, optional    | Mature Rust work-stealing runtime comparison |

Example workloads:

* many small CPU-bound tasks
* fewer large CPU-bound tasks
* uneven task durations
* recursive fork-join workload
* producer-heavy submission workload
* dependency graph workload

Example benchmark output:

```text
workload: many_small_tasks
tasks:    100000
workers:  8

single-threaded:      420 ms
thread-per-task:     1850 ms
global-queue pool:    160 ms
work-stealing pool:   105 ms
```

## Testing Strategy

The project should include both correctness tests and stress tests.

Correctness tests:

* submitted tasks eventually run
* task handles receive results
* worker shutdown is clean
* cancelled tasks observe cancellation
* dependency tasks run in the correct order
* priority tasks are scheduled ahead of lower-priority work when possible

Stress tests:

* many concurrent submissions
* nested task spawning
* long-running tasks mixed with short tasks
* repeated create/shutdown cycles
* high-contention workloads
* work stealing under uneven load

Optional tools:

```bash
cargo test
cargo clippy
cargo bench
RUST_BACKTRACE=1 cargo test
```

## Design Notes

### Why a fixed-size thread pool?

Creating one thread per task is expensive. A fixed-size worker pool limits thread creation overhead and makes CPU usage more predictable.

### Why work stealing?

A single global queue is simple, but it can become a contention point. Work stealing lets workers mostly operate on local queues while still balancing work when some workers become idle.

### Why cooperative cancellation?

Forcibly stopping threads is unsafe. Cooperative cancellation allows tasks to check a token and exit cleanly.

### Why benchmark multiple schedulers?

A runtime design should be measured, not assumed. Different workloads favor different scheduling strategies, so the benchmark suite compares the tradeoffs directly.

## Rust Concepts Used

This project is intended to exercise several Rust systems-programming concepts:

* ownership and move semantics
* `Send` and `Sync`
* `Arc`
* `Mutex`
* `Condvar`
* channels
* atomics
* worker thread lifecycle management
* trait bounds for task closures
* error handling with `Result`
* benchmarking and profiling

## Example Learning Outcomes

After completing this project, the implementation should demonstrate:

* how thread pools execute tasks without creating one thread per task
* how task handles deliver results back to callers
* how workers coordinate shutdown
* how work stealing improves load balancing
* how Rust prevents unsafe sharing across threads
* how runtime design choices affect throughput and latency

## Roadmap

### Phase 1: Basic Runtime

* [ ] create fixed worker pool
* [ ] submit `FnOnce + Send + 'static` tasks
* [ ] execute tasks from shared queue
* [ ] support graceful shutdown

### Phase 2: Result Handles

* [ ] return task handle from `spawn`
* [ ] support `join`
* [ ] propagate panics or task errors
* [ ] test result delivery

### Phase 3: Work Stealing

* [ ] add per-worker queues
* [ ] implement stealing from other workers
* [ ] benchmark against global queue
* [ ] document scheduling tradeoffs

### Phase 4: Advanced Scheduling

* [ ] add task priorities
* [ ] add cooperative cancellation
* [ ] add task timeouts
* [ ] add dependency graph scheduling

### Phase 5: Observability and Polish

* [ ] add runtime metrics
* [ ] add benchmark charts
* [ ] add architecture diagram
* [ ] add CI workflow
* [ ] add design documentation

## Possible Future Extensions

* async task adapter
* NUMA-aware worker placement
* CPU affinity experiments
* tracing integration
* flamegraph profiling
* bounded queues and backpressure
* scheduler visualization
* integration with a mini build system
* integration with a mini job orchestrator

## Related Concepts

This project is related to:

* work-stealing schedulers
* fork-join parallelism
* task runtimes
* CPU-bound parallel execution
* build-system schedulers
* job orchestration
* cooperative cancellation
* concurrent queues
* runtime benchmarking

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
