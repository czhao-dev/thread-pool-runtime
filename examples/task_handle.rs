use work_stealing_thread_pool::Runtime;

fn main() {
    let runtime = Runtime::new(4);

    let handle = runtime.spawn(|| "hello from worker");
    let value = handle.join().unwrap();
    assert_eq!(value, "hello from worker");
    println!("got: {value}");

    let panicking = runtime.spawn(|| -> u32 { panic!("deliberate failure") });
    match panicking.join() {
        Ok(_) => unreachable!(),
        Err(err) => println!("task failed as expected: {err}"),
    }

    runtime.shutdown();
}
