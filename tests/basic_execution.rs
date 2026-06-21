use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use rust_thread_pool_runtime::Runtime;

#[test]
fn submitted_tasks_eventually_run() {
    let runtime = Runtime::new(4);
    let counter = Arc::new(AtomicUsize::new(0));

    let handles: Vec<_> = (0..500)
        .map(|_| {
            let counter = counter.clone();
            runtime.spawn(move || {
                counter.fetch_add(1, Ordering::SeqCst);
            })
        })
        .collect();

    for handle in handles {
        handle.join().unwrap();
    }

    assert_eq!(counter.load(Ordering::SeqCst), 500);
    runtime.shutdown();
}

#[test]
fn task_handles_receive_results() {
    let runtime = Runtime::new(4);

    let handle = runtime.spawn(|| 6 * 7);
    assert_eq!(handle.join().unwrap(), 42);

    let handle = runtime.spawn(|| "value".to_string());
    assert_eq!(handle.join().unwrap(), "value");

    runtime.shutdown();
}

#[test]
fn panicking_tasks_report_a_join_error() {
    let runtime = Runtime::new(2);

    let handle = runtime.spawn(|| -> u32 { panic!("boom") });
    let err = handle.join().unwrap_err();
    assert!(err.to_string().contains("boom"));

    // The pool must still be healthy after a panic.
    let handle = runtime.spawn(|| 1 + 1);
    assert_eq!(handle.join().unwrap(), 2);

    runtime.shutdown();
}

#[test]
fn metrics_track_submitted_and_completed_tasks() {
    let runtime = Runtime::new(4);

    for _ in 0..50 {
        runtime.spawn(|| ()).join().unwrap();
    }

    let metrics = runtime.metrics();
    assert_eq!(metrics.tasks_submitted, 50);
    assert_eq!(metrics.tasks_completed, 50);
    assert_eq!(metrics.tasks_panicked, 0);

    runtime.shutdown();
}

#[test]
fn nested_task_spawning_completes() {
    let runtime = Arc::new(Runtime::new(4));
    let inner_runtime = runtime.clone();

    let handle = runtime.spawn(move || {
        let child = inner_runtime.spawn(|| 41);
        child.join().unwrap() + 1
    });

    assert_eq!(handle.join().unwrap(), 42);
    runtime.shutdown();
}
