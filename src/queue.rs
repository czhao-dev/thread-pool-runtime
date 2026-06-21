//! A plain shared-mutex thread pool, with no local queues and no stealing.
//!
//! `GlobalQueuePool` exists purely as a baseline for the benchmark suite:
//! it answers "how much does work stealing actually buy us?" by giving the
//! work-stealing [`Runtime`](crate::Runtime) something architecturally
//! simple to be measured against.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;

use crate::handle::{new_handle_pair, panic_message, JoinHandle};
use crate::task::Job;

struct Shared {
    queue: Mutex<VecDeque<Job>>,
    cond: Condvar,
    shutdown: AtomicBool,
}

/// A fixed-size pool of workers pulling from one shared, mutex-guarded
/// FIFO queue.
pub struct GlobalQueuePool {
    shared: Arc<Shared>,
    workers: Mutex<Option<Vec<thread::JoinHandle<()>>>>,
}

impl GlobalQueuePool {
    /// Creates a pool with `num_workers` worker threads.
    pub fn new(num_workers: usize) -> Self {
        assert!(num_workers > 0, "a pool needs at least one worker thread");

        let shared = Arc::new(Shared {
            queue: Mutex::new(VecDeque::new()),
            cond: Condvar::new(),
            shutdown: AtomicBool::new(false),
        });

        let workers = (0..num_workers)
            .map(|id| {
                let shared = shared.clone();
                thread::Builder::new()
                    .name(format!("global-queue-worker-{id}"))
                    .spawn(move || Self::worker_loop(shared))
                    .expect("failed to spawn worker thread")
            })
            .collect();

        Self {
            shared,
            workers: Mutex::new(Some(workers)),
        }
    }

    fn worker_loop(shared: Arc<Shared>) {
        loop {
            let job = {
                let mut queue = shared.queue.lock().unwrap();
                loop {
                    if let Some(job) = queue.pop_front() {
                        break Some(job);
                    }
                    if shared.shutdown.load(Ordering::Acquire) {
                        break None;
                    }
                    queue = shared.cond.wait(queue).unwrap();
                }
            };
            match job {
                Some(job) => job(),
                None => break,
            }
        }
    }

    /// Submits a task and returns a handle to its result.
    pub fn spawn<F, T>(&self, f: F) -> JoinHandle<T>
    where
        F: FnOnce() -> T + Send + 'static,
        T: Send + 'static,
    {
        let (setter, handle) = new_handle_pair();
        let job: Job =
            Box::new(
                move || match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
                    Ok(value) => setter.set(Ok(value)),
                    Err(payload) => setter.set(Err(panic_message(payload))),
                },
            );
        {
            let mut queue = self.shared.queue.lock().unwrap();
            queue.push_back(job);
        }
        self.shared.cond.notify_one();
        handle
    }

    /// Stops accepting the premise that more work is coming, drains the
    /// queue, and joins all worker threads. Safe to call multiple times.
    pub fn shutdown(&self) {
        self.shared.shutdown.store(true, Ordering::SeqCst);
        self.shared.cond.notify_all();
        if let Some(workers) = self.workers.lock().unwrap().take() {
            for worker in workers {
                let _ = worker.join();
            }
        }
    }
}

impl Drop for GlobalQueuePool {
    fn drop(&mut self) {
        self.shutdown();
    }
}
