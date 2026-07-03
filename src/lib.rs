//! A from-scratch, work-stealing task scheduler.
//!
//! The primary entry point is [`Runtime`]: a fixed-size worker pool with
//! per-priority work-stealing queues. Built on top of it are cooperative
//! [`cancellation`], a [`TaskGraph`] dependency scheduler, and runtime
//! [`RuntimeMetrics`] counters. [`GlobalQueuePool`] is a deliberately
//! simple single-queue pool kept around as a benchmark baseline.
//!
//! See the repository README for the architecture write-up and benchmark
//! results.

mod cancellation;
mod dependency;
mod handle;
mod metrics;
mod priority;
mod queue;
mod runtime;
mod steal;
mod task;
mod worker;

pub use cancellation::{CancellationContext, CancellationToken};
pub use dependency::{run_graph, DependencyResults, NodeId, TaskGraph};
pub use handle::{JoinError, JoinHandle};
pub use metrics::RuntimeMetrics;
pub use priority::Priority;
pub use queue::GlobalQueuePool;
pub use runtime::Runtime;
