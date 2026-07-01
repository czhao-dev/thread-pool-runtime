use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use work_stealing_thread_pool::Runtime;

#[test]
fn shutdown_waits_for_in_flight_tasks_to_finish() {
    let runtime = Runtime::new(4);
    let completed = Arc::new(AtomicUsize::new(0));

    for _ in 0..32 {
        let completed = completed.clone();
        runtime.spawn(move || {
            std::thread::sleep(Duration::from_millis(5));
            completed.fetch_add(1, Ordering::SeqCst);
        });
    }

    runtime.shutdown();
    assert_eq!(completed.load(Ordering::SeqCst), 32);
}

#[test]
fn shutdown_is_idempotent() {
    let runtime = Runtime::new(2);
    runtime.spawn(|| ()).join().unwrap();
    runtime.shutdown();
    runtime.shutdown();
}

#[test]
fn repeated_create_and_shutdown_cycles_are_clean() {
    for _ in 0..20 {
        let runtime = Runtime::new(2);
        let handle = runtime.spawn(|| 1 + 1);
        assert_eq!(handle.join().unwrap(), 2);
        runtime.shutdown();
    }
}

#[test]
fn dropping_runtime_without_explicit_shutdown_still_runs_tasks() {
    let completed = Arc::new(AtomicUsize::new(0));
    {
        let runtime = Runtime::new(2);
        for _ in 0..10 {
            let completed = completed.clone();
            runtime.spawn(move || {
                completed.fetch_add(1, Ordering::SeqCst);
            });
        }
        // runtime dropped here without calling shutdown()
    }
    assert_eq!(completed.load(Ordering::SeqCst), 10);
}
