use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use work_stealing_thread_pool::{run_graph, Priority, Runtime, TaskGraph};

#[test]
fn many_concurrent_submissions_from_multiple_threads() {
    let runtime = Arc::new(Runtime::new(8));
    let completed = Arc::new(AtomicUsize::new(0));

    let submitters: Vec<_> = (0..8)
        .map(|_| {
            let runtime = runtime.clone();
            let completed = completed.clone();
            std::thread::spawn(move || {
                let handles: Vec<_> = (0..500)
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
            })
        })
        .collect();

    for submitter in submitters {
        submitter.join().unwrap();
    }

    assert_eq!(completed.load(Ordering::SeqCst), 4000);
    runtime.shutdown();
}

#[test]
fn long_running_tasks_mixed_with_short_tasks() {
    let runtime = Runtime::new(4);

    let long = runtime.spawn(|| {
        std::thread::sleep(Duration::from_millis(50));
        "long done"
    });

    let shorts: Vec<_> = (0..2_000).map(|i| runtime.spawn(move || i * 2)).collect();

    for (i, handle) in shorts.into_iter().enumerate() {
        assert_eq!(handle.join().unwrap(), i * 2);
    }
    assert_eq!(long.join().unwrap(), "long done");

    runtime.shutdown();
}

#[test]
fn priority_tasks_are_preferred_over_background_work() {
    let runtime = Runtime::new(1);
    let order = Arc::new(AtomicUsize::new(0));

    // Saturate the single worker's queue with background work first.
    let background_handles: Vec<_> = (0..500)
        .map(|_| {
            let order = order.clone();
            runtime.spawn_with_priority(Priority::Background, move || {
                order.fetch_add(1, Ordering::SeqCst)
            })
        })
        .collect();

    let high_order = order.clone();
    let high = runtime.spawn_with_priority(Priority::High, move || {
        high_order.fetch_add(1, Ordering::SeqCst)
    });

    let high_rank = high.join().unwrap();
    for handle in background_handles {
        handle.join().unwrap();
    }

    assert!(
        high_rank < 500,
        "high-priority task ran at position {high_rank}, expected it to cut ahead of most background work"
    );

    runtime.shutdown();
}

#[test]
fn dependency_graph_runs_tasks_in_correct_order() {
    let runtime = Runtime::new(4);
    let mut graph = TaskGraph::new();

    let a = graph.add_task(|| 10usize);
    let b = graph.add_task(|| 20usize);
    let c = graph.add_task_after([a, b], move |results| {
        let x: usize = results.get(a);
        let y: usize = results.get(b);
        x + y
    });
    let d = graph.add_task_after([c], move |results| {
        let sum: usize = results.get(c);
        sum * 2
    });

    let results = run_graph(&runtime, graph);
    assert_eq!(results.get::<usize>(a), 10);
    assert_eq!(results.get::<usize>(b), 20);
    assert_eq!(results.get::<usize>(c), 30);
    assert_eq!(results.get::<usize>(d), 60);

    runtime.shutdown();
}

#[test]
fn wide_dependency_graph_completes() {
    let runtime = Runtime::new(8);
    let mut graph = TaskGraph::new();

    let roots: Vec<_> = (0..200usize).map(|i| graph.add_task(move || i)).collect();
    let total = graph.add_task_after(roots.clone(), move |results| {
        roots
            .iter()
            .map(|&id| results.get::<usize>(id))
            .sum::<usize>()
    });

    let results = run_graph(&runtime, graph);
    assert_eq!(results.get::<usize>(total), (0..200).sum::<usize>());

    runtime.shutdown();
}

#[test]
fn repeated_pool_create_and_shutdown_under_load() {
    for _ in 0..10 {
        let runtime = Runtime::new(4);
        let handles: Vec<_> = (0..200).map(|i| runtime.spawn(move || i + 1)).collect();
        for (i, handle) in handles.into_iter().enumerate() {
            assert_eq!(handle.join().unwrap(), i + 1);
        }
        runtime.shutdown();
    }
}
