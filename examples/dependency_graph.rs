use work_stealing_thread_pool::{run_graph, Runtime, TaskGraph};

fn parse_file(name: &str) -> usize {
    println!("parsing {name}");
    name.len()
}

fn main() {
    let runtime = Runtime::new(4);
    let mut graph = TaskGraph::new();

    let a = graph.add_task(|| parse_file("a.rs"));
    let b = graph.add_task(|| parse_file("b.rs"));

    let c = graph.add_task_after([a, b], move |results| {
        let len_a: usize = results.get(a);
        let len_b: usize = results.get(b);
        println!("linking outputs: {len_a} + {len_b} bytes");
        len_a + len_b
    });

    let results = run_graph(&runtime, graph);
    println!("link_outputs() = {}", results.get::<usize>(c));

    runtime.shutdown();
}
