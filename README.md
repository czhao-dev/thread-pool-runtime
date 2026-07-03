# Work-Stealing Scheduler

[![Rust](https://img.shields.io/badge/Rust-2021-CE412B?logo=rust&logoColor=white)](https://www.rust-lang.org)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![crossbeam-deque](https://img.shields.io/badge/crossbeam--deque-0.8-orange)](https://crates.io/crates/crossbeam-deque)
[![criterion](https://img.shields.io/badge/criterion-0.5-orange)](https://crates.io/crates/criterion)

A from-scratch work-stealing scheduler written in Rust: a fixed-size worker pool with per-priority work-stealing queues, panic-safe task handles, cooperative cancellation, and a dependency-graph (DAG) task scheduler built on top of it.

This is not meant to replace mature libraries such as Rayon or Tokio. It's a learning-focused implementation of the mechanisms behind CPU task runtimes, job schedulers, and work-stealing execution engines — built to be read, benchmarked, and reasoned about.

## Overview

`work-stealing-scheduler` starts every worker thread once and keeps it alive for the life of the pool. Tasks are short-lived closures (`FnOnce() -> T + Send`) that get scheduled onto those workers through a set of work-stealing queues, one per priority level. On top of that scheduling core, the crate adds:

* task submission with panic-safe result handles (`spawn`, `JoinHandle<T>`)
* three priority classes, honored at every stage of scheduling
* cooperative cancellation via a shared, pollable token
* a dependency-graph executor (`TaskGraph` / `run_graph`) for DAG-shaped workloads
* runtime counters (submitted / completed / panicked / stolen task counts)
* a benchmark suite comparing the work-stealing pool against three simpler execution strategies

## Why Rust

Thread pools are everywhere in backend systems, build systems, compilers, storage engines, and distributed services, and a good one has to balance several things at once: keep cores busy, avoid excessive thread creation, minimize scheduling overhead, avoid idle workers while work exists elsewhere, and shut down cleanly. Rust forces the implementation to be explicit about ownership, lifetimes, and thread safety while building exactly that — `Send`/`Sync` bounds on task closures, `Arc`-shared scheduler state, and panic unwinding across thread boundaries are all enforced by the type system rather than by convention.

## Architecture

```text
                          Runtime
                 (spawn, spawn_with_priority,
                  spawn_cancellable, shutdown)
                            │
                            │ push(priority, job)
                            ▼
              ┌───────────────────────────┐
              │   Injectors (1 per         │
              │   priority: High/Normal/   │
              │   Background)              │
              └─────────────┬──────────────┘
                 steal_batch_and_pop      steal_batch_and_pop
                            │                          │
                ┌───────────▼───────────┐  ┌───────────▼───────────┐
                │  Worker 0              │  │  Worker N              │
                │  local deques          │◄─┤  local deques          │
                │  (High/Normal/Bg)      │  │  (High/Normal/Bg)      │
                │  execute, pop()        │─►│  steal()               │
                └────────────────────────┘  └────────────────────────┘
```

### Scheduling core (`steal.rs`, `worker.rs`, `runtime.rs`)

Each priority level (`High`, `Normal`, `Background`) gets its own independent set of queues, so a flood of background work can never delay a high-priority task. Concretely, that's three [`crossbeam_deque::Injector`](https://docs.rs/crossbeam-deque)s (one global, multi-producer queue per priority) and, per worker, three local single-owner deques plus their `Stealer` handles.

`Runtime::spawn*` always pushes onto the appropriate priority's injector — that's the "global queue" external callers submit into. A worker looking for work runs this search, in priority order at every step:

1. **Own local queues** — pop from its own High deque, then Normal, then Background.
2. **Global injectors** — `steal_batch_and_pop` from each priority's injector in turn. This both grabs a task to run *and* moves a batch of further tasks into the worker's own local deque, which is what gives later iterations cache-friendly, contention-free access to that work.
3. **Peer workers** — `steal()` a single task directly from another worker's local deque, again in priority order, skipping itself.

This three-stage search is the standard shape of a work-stealing scheduler — the same one Rayon and Tokio's multi-threaded scheduler are built on: workers run almost entirely off their own queue, batches absorbed from the injector amortize the cost of going to shared state, and direct peer-stealing is the fallback that keeps a pool from idling while work sits unevenly distributed on another worker. Running 50,000 trivial tasks across 8 workers, **24% of tasks (12,021 of 50,000) were picked up via a direct peer steal** rather than a local pop or injector pull — concrete evidence the steal path isn't just theoretical, it's load-bearing under real submission patterns.

An idle worker doesn't spin: it waits on a `Condvar`-based doorbell (`IdleSignal`) that every push and every task completion notifies, with a 1ms timeout as a correctness fallback in case a wakeup is ever missed — this bounds worst-case scheduling latency without busy-polling.

**Shutdown and lifetime.** An `AtomicUsize` counts tasks that have been submitted but not yet completed (incremented on submit, decremented after a job runs — including any panic). A worker only exits once `shutdown` has been requested *and* that counter is zero, which means a task that spawns child tasks from inside its own body can never race a shutdown into stopping early: the counter is bumped for the child before the parent's own decrement fires. `Runtime` also shuts itself down on `Drop`, so forgetting to call `shutdown()` explicitly leaks nothing.

### Task handles and panics (`handle.rs`, `task.rs`)

Every queue and every steal operation moves the same concrete type, `Box<dyn FnOnce() + Send + 'static>` — a `Job`. Type erasure happens once, at `spawn` time: the closure that gets boxed wraps the user's `FnOnce() -> T` in `std::panic::catch_unwind`, and delivers `Ok(T)` or `Err(message)` through a small `Arc<Mutex<Option<Result<T, String>>>> + Condvar` pair (`JoinHandle<T>` / `ResultSetter<T>`). A panic on a worker thread is caught, recorded in the runtime's metrics, and surfaced to the caller as a normal `Result` from `.join()` — it never takes down the worker thread or the pool.

### Priority (`priority.rs`)

`Priority` is a 3-variant, `#[repr(usize)]` enum used directly as an array index everywhere (`PRIORITY_ORDER`, the three injectors, the three local deques), so picking "the next priority to check" is a fixed-size array walk with no branching on enum discriminants.

### Cancellation (`cancellation.rs`)

The runtime never forcibly kills a thread mid-task — doing so while it might be holding a lock or mid-destructor would be unsound. `CancellationToken` is instead a shareable `Arc<AtomicBool>`; `spawn_cancellable` hands the task body a `CancellationContext` it can poll. Cancellation is purely cooperative: it's a suggestion the task can check on its own terms (typically inside a loop), the same model `std::sync::atomic` + a polling flag gives you in any language, just wrapped so the call site reads like a first-class runtime feature.

### Dependency graph (`dependency.rs`)

`TaskGraph` is a DAG built by `add_task` (no dependencies) and `add_task_after` (depends on previously-returned `NodeId`s). Because a `NodeId` can only ever reference a node added earlier, **cycles are impossible by construction** — there's no validation step because there's nothing to validate.

`run_graph` is deliberately *not* built into `Runtime` itself. It's a scheduler written entirely against the public `spawn_with_priority` API: it computes remaining-dependency counts and reverse edges, submits every node with zero dependencies, and then drives the rest from an `mpsc::Receiver` that each node's wrapper closure reports its index into on completion. Receiving a completion decrements its dependents' counters on the calling thread (no atomics needed — only one thread ever touches that count) and submits any dependent that just hit zero. This sidesteps a real borrow-checker constraint: a task spawned onto the pool must be `'static`, so a node's closure can't recursively capture `&Runtime` to spawn its own dependents from a worker thread. Routing completions back through a channel to the single thread that holds the `&Runtime` borrow (the one blocked inside `run_graph`) solves it without `unsafe` or reaching for `Arc<Runtime>` everywhere.

A dependent task reads its prerequisites' outputs through `DependencyResults::get::<T>`, which downcasts a type-erased `Box<dyn Any + Send>` stored per node. It's dynamic typing at the boundary in exchange for letting graph nodes return whatever type makes sense for that node — the same tradeoff build systems and job graphs with heterogeneous outputs make routinely.

### Baseline pool (`queue.rs`)

`GlobalQueuePool` is a second, intentionally simple pool: one `Mutex<VecDeque<Job>>` and a `Condvar`, no local queues, no stealing. It exists purely so the benchmark suite has something architecturally simple to measure the work-stealing pool against.

## Benchmarks

Measured with `criterion` (20 samples/benchmark) on an Apple M3 (8 cores), `rustc 1.96.0`, release profile (`opt-level = 3`, `lto = true`, `codegen-units = 1`). Each task body is a busy CPU loop seeded per-task so the optimizer can't fold repeated calls into a constant — see [`benches/scheduler_bench.rs`](benches/scheduler_bench.rs). Reproduce with `cargo bench`.

| Workload | single-threaded | thread-per-task | global-queue pool | work-stealing pool |
| --- | --- | --- | --- | --- |
| `many_small_tasks` (2,000 tasks × ~2k ops) | 4.96 ms | 21.6 ms | **1.40 ms** | 1.93 ms |
| `fewer_large_tasks` (64 tasks × ~2M ops) | 161.5 ms | 27.3 ms | 27.3 ms | **27.5 ms** |
| `uneven_durations` (500 tasks, 1-in-10 ~100x heavier) | 126.9 ms | 23.6 ms | **22.3 ms** | 22.5 ms |
| `dependency_graph` (fan-out 200 → join, work-stealing only) | — | — | — | 0.30 ms |

All eight pool workers; thread-per-task spawns one OS thread per task.

**What this shows:**

* **Thread-per-task is the clear loser for many small tasks** (21.6ms vs. ~1.4-1.9ms): OS thread creation costs roughly 10µs+ per thread, and with 2,000 tiny tasks that overhead dwarfs the actual work — exactly the problem fixed-size pools exist to solve.
* **For CPU-bound work large enough to amortize scheduling overhead, all three threaded strategies converge** (~27ms for `fewer_large_tasks`, all within 1% of each other) — at that point the work itself, not the scheduler, is the bottleneck, and single-threaded execution simply doesn't have the cores to compete (161ms vs. ~27ms, roughly the expected ~6x from 8 cores after non-parallelizable overhead).
* **The simple global-queue pool is not slower than the work-stealing pool here, and is sometimes faster** (`many_small_tasks`, `uneven_durations`). At 8 workers and these task counts, the constant overhead of crossbeam's local-deque/batch-steal machinery costs slightly more than a single mutex saves in reduced contention — a real, somewhat counter-intuitive result, and a good reminder that work stealing is a tool for *avoiding starvation under skew*, not a universal throughput win. Its advantage shows up under contention and uneven scheduling, not in `criterion`'s clean, repeated-trial loop where a single mutex rarely blocks long enough to matter. The 24% peer-steal rate measured separately (see Architecture) confirms the mechanism is active; it just isn't the bottleneck in these particular workloads.
* **The dependency graph adds negligible overhead**: scheduling 200 fan-out tasks plus a join through `TaskGraph` takes 0.3ms, almost all of it the same per-task submission cost already paid by `many_small_tasks`.

## Quick Start

```bash
git clone https://github.com/czhao-dev/work-stealing-thread-pool.git
cd work-stealing-thread-pool
cargo test                                          # 21 tests across 5 files
cargo run --example basic_pool
cargo bench
cargo fmt && cargo clippy --all-targets --all-features -- -D warnings
```

## Repository Layout

```text
work-stealing-thread-pool/
├── Cargo.toml
├── benches/
│   └── scheduler_bench.rs      # criterion comparison across 4 strategies
├── examples/
│   ├── basic_pool.rs
│   ├── task_handle.rs
│   ├── priority_tasks.rs
│   └── dependency_graph.rs
├── src/
│   ├── lib.rs
│   ├── runtime.rs              # public Runtime API, shutdown, metrics wiring
│   ├── worker.rs               # worker main loop, idle doorbell
│   ├── steal.rs                # per-priority work-stealing queues
│   ├── task.rs                 # the type-erased Job alias
│   ├── handle.rs               # JoinHandle / panic-safe results
│   ├── priority.rs             # Priority enum and scan order
│   ├── cancellation.rs         # CancellationToken / Context
│   ├── dependency.rs           # TaskGraph / run_graph
│   ├── metrics.rs              # runtime counters
│   └── queue.rs                # GlobalQueuePool benchmark baseline
└── tests/
    ├── basic_execution.rs
    ├── shutdown.rs
    ├── cancellation.rs
    ├── work_stealing.rs
    └── stress.rs
```

## Testing Strategy

21 integration tests across five files, run with `cargo test`.

* **`basic_execution.rs`** — tasks run, handles deliver results, panics surface as `JoinError` without poisoning the pool, metrics stay consistent, nested spawning from inside a task completes.
* **`shutdown.rs`** — shutdown drains in-flight work before joining, is idempotent, survives repeated create/shutdown cycles, and runs cleanly even via `Drop` if `shutdown()` is never called explicitly.
* **`cancellation.rs`** — a running task observes cancellation and exits cooperatively; cancelling before a task starts is observed immediately; an unrelated task on a cancelled token is unaffected.
* **`work_stealing.rs`** — other workers keep making progress while one is blocked; a 20,000-task fan-out completes and is fully accounted for in the metrics; nested fan-out from inside a task completes.
* **`stress.rs`** — concurrent submission from 8 producer threads, long-running tasks interleaved with thousands of short ones, priority preference under saturation, dependency graphs (linear chain and 200-wide fan-out/join), and repeated pool lifecycles under load.

## Design Notes

**Why a fixed-size thread pool?** Spawning one OS thread per task is expensive — the `thread_per_task` benchmark above pays for this directly. A fixed-size worker pool bounds thread creation to pool startup and makes CPU usage predictable regardless of task count.

**Why per-priority work stealing instead of one shared queue?** A single global queue is simple and, as the benchmarks show, often competitive — but it's a single point of contention, and it has no answer for one worker ending up starved while another sits on a backlog. Per-worker local queues plus stealing keep the common case (a worker running off its own queue) free of contention while still rebalancing when work is unevenly distributed, which is precisely the situation a single queue handles worst.

**Why separate queues per priority instead of a priority heap?** A shared priority queue needs a lock (or a lock-free skip-list-like structure) and reintroduces exactly the contention work stealing is meant to avoid. Three independent sets of plain FIFO work-stealing queues, scanned high-to-low, get strict priority ordering without giving up any of the stealing architecture.

**Why cooperative cancellation?** Forcibly stopping a thread mid-task is unsound in the presence of locks, destructors, and unfinished writes. A pollable token lets a task decide when it's safe to stop.

**Why benchmark four strategies instead of asserting work stealing is best?** Because it isn't, unconditionally — these results show the global-queue pool keeping pace with (and twice beating) the work-stealing pool at this core count and these workloads. A runtime design should be measured against real alternatives, not assumed superior because it's more sophisticated.

## Rust Concepts Used

* ownership and move semantics across thread boundaries
* `Send` and `Sync` bounds on task closures and shared state
* `Arc`, `Mutex`, `Condvar`, `AtomicBool`/`AtomicUsize`
* `std::sync::mpsc` channels (dependency graph completion reporting)
* lock-free work-stealing deques (`crossbeam-deque`)
* `std::panic::catch_unwind` / `AssertUnwindSafe` for panic-safe task results
* trait bounds for type-erased task closures (`Box<dyn FnOnce() + Send + 'static>`)
* dynamic typing via `Box<dyn Any + Send>` and downcasting
* benchmarking with `criterion`

## References

**Foundational papers**

- Blumofe, R. D. & Leiserson, C. E. (1999). Scheduling multithreaded computations by work stealing. *Journal of the ACM*, 46(5), 720–748. <https://dl.acm.org/doi/10.1145/324133.324234>
- Chase, D. & Lev, Y. (2005). Dynamic circular work-stealing deque. *Proceedings of the 17th Annual ACM Symposium on Parallelism in Algorithms and Architectures (SPAA '05)*, 21–28. <https://dl.acm.org/doi/10.1145/1073970.1073974>

**Crates**

- [`crossbeam-deque`](https://docs.rs/crossbeam-deque) — lock-free work-stealing deque; the scheduling primitive this runtime is built on.
- [`criterion`](https://bheisler.github.io/criterion.rs/book/) — statistics-driven micro-benchmarking for Rust.
- [Rayon](https://github.com/rayon-rs/rayon) — production data-parallelism library for Rust; a reference point for work-stealing design at scale.
- [Tokio](https://tokio.rs) — async runtime for Rust; its multi-threaded scheduler uses the same work-stealing architecture.

**Rust references**

- [The Rust Programming Language](https://doc.rust-lang.org/book/) — authoritative guide to ownership, `Send`/`Sync`, and concurrency primitives.
- [The Rustonomicon](https://doc.rust-lang.org/nomicon/) — the unsafe Rust guide; covers `catch_unwind`, `AssertUnwindSafe`, and the soundness requirements relevant to panic-safe task execution.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
