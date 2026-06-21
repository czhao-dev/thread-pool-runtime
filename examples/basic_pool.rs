use rust_thread_pool_runtime::Runtime;

fn main() {
    let runtime = Runtime::new(4);

    let handle = runtime.spawn(|| {
        let mut sum = 0u64;
        for i in 0..1_000_000u64 {
            sum += i;
        }
        sum
    });

    let result = handle.join().unwrap();
    println!("result = {result}");

    runtime.shutdown();
}
