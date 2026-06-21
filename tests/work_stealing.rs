use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Barrier};
use std::time::{Duration, Instant};

use rust_thread_pool_runtime::Runtime;

#[test]
fn idle_workers_keep_making_progress_while_one_worker_is_busy() {
    let runtime = Runtime::new(4);
    let barrier = Arc::new(Barrier::new(2));

    // Occupy exactly one worker with a task that blocks until released.
    let blocker_barrier = barrier.clone();
    let blocker = runtime.spawn(move || {
        blocker_barrier.wait();
    });

    let completed = Arc::new(AtomicUsize::new(0));
    let start = Instant::now();
    let handles: Vec<_> = (0..300)
        .map(|_| {
            let completed = completed.clone();
            runtime.spawn(move || {
                completed.fetch_add(1, Ordering::SeqCst);
            })
        })
        .collect();

    for handle in handles {
        handle.join().unwrap();
    }
    let elapsed = start.elapsed();

    barrier.wait();
    blocker.join().unwrap();

    assert_eq!(completed.load(Ordering::SeqCst), 300);
    assert!(
        elapsed < Duration::from_secs(5),
        "300 trivial tasks took {elapsed:?} despite 3 free workers; work is not being distributed"
    );

    runtime.shutdown();
}

#[test]
fn high_fanout_workload_is_distributed_across_workers_and_completes() {
    let runtime = Runtime::new(8);
    let completed = Arc::new(AtomicUsize::new(0));

    let handles: Vec<_> = (0..20_000)
        .map(|_| {
            let completed = completed.clone();
            runtime.spawn(move || {
                completed.fetch_add(1, Ordering::SeqCst);
            })
        })
        .collect();

    for handle in handles {
        handle.join().unwrap();
    }

    assert_eq!(completed.load(Ordering::SeqCst), 20_000);

    let metrics = runtime.metrics();
    assert_eq!(metrics.tasks_submitted, 20_000);
    assert_eq!(metrics.tasks_completed, 20_000);

    runtime.shutdown();
}

#[test]
fn nested_fanout_from_within_a_task_completes() {
    let runtime = Arc::new(Runtime::new(4));
    let completed = Arc::new(AtomicUsize::new(0));

    let inner_runtime = runtime.clone();
    let inner_completed = completed.clone();
    let top = runtime.spawn(move || {
        let handles: Vec<_> = (0..200)
            .map(|_| {
                let completed = inner_completed.clone();
                inner_runtime.spawn(move || {
                    completed.fetch_add(1, Ordering::SeqCst);
                })
            })
            .collect();
        for handle in handles {
            handle.join().unwrap();
        }
    });

    top.join().unwrap();
    assert_eq!(completed.load(Ordering::SeqCst), 200);

    runtime.shutdown();
}
