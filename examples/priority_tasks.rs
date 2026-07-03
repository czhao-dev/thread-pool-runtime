use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use work_stealing_scheduler::{Priority, Runtime};

/// Floods the pool with background work, then submits a high-priority task
/// and confirms it still completes promptly by recording the order tasks
/// actually ran in.
fn main() {
    let runtime = Runtime::new(2);
    let order = Arc::new(AtomicUsize::new(0));
    let high_priority_rank = Arc::new(AtomicUsize::new(usize::MAX));

    let mut background_handles = Vec::new();
    for _ in 0..200 {
        let order = order.clone();
        background_handles.push(runtime.spawn_with_priority(Priority::Background, move || {
            order.fetch_add(1, Ordering::SeqCst);
            std::thread::yield_now();
        }));
    }

    let rank = high_priority_rank.clone();
    let order_for_high = order.clone();
    let high = runtime.spawn_with_priority(Priority::High, move || {
        rank.store(
            order_for_high.fetch_add(1, Ordering::SeqCst),
            Ordering::SeqCst,
        );
    });

    high.join().unwrap();
    for handle in background_handles {
        handle.join().unwrap();
    }

    println!(
        "high-priority task finished as the #{} task to run out of 201",
        high_priority_rank.load(Ordering::SeqCst) + 1
    );

    runtime.shutdown();
}
