use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use work_stealing_scheduler::{CancellationToken, Runtime};

#[test]
fn cancelled_tasks_observe_cancellation() {
    let runtime = Runtime::new(2);
    let token = CancellationToken::new();
    let iterations = Arc::new(AtomicUsize::new(0));

    let worker_iterations = iterations.clone();
    let handle = runtime.spawn_cancellable(token.clone(), move |ctx| {
        while !ctx.is_cancelled() {
            worker_iterations.fetch_add(1, Ordering::SeqCst);
            std::thread::sleep(Duration::from_millis(1));
        }
        "stopped cooperatively"
    });

    std::thread::sleep(Duration::from_millis(20));
    token.cancel();

    assert_eq!(handle.join().unwrap(), "stopped cooperatively");
    assert!(iterations.load(Ordering::SeqCst) > 0);

    runtime.shutdown();
}

#[test]
fn cancelling_before_the_task_starts_is_observed_immediately() {
    let runtime = Runtime::new(1);
    let token = CancellationToken::new();
    token.cancel();

    let handle = runtime.spawn_cancellable(token, |ctx| ctx.is_cancelled());
    assert!(handle.join().unwrap());

    runtime.shutdown();
}

#[test]
fn unrelated_tasks_are_unaffected_by_a_cancelled_token() {
    let runtime = Runtime::new(2);
    let token = CancellationToken::new();
    token.cancel();

    runtime.spawn_cancellable(token, |_ctx| ()).join().unwrap();

    let handle = runtime.spawn(|| 5 * 5);
    assert_eq!(handle.join().unwrap(), 25);

    runtime.shutdown();
}
