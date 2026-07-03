#pragma once

#include <thread>

namespace wss::affinity {

// Best-effort core pinning.
//
// Linux: pthread_setaffinity_np — a hard pin, honored by the kernel
// scheduler.
//
// macOS: thread_policy_set with THREAD_AFFINITY_POLICY — a scheduler HINT
// only. There is no supported hard-pinning API on macOS, and Apple Silicon
// in particular gives no pinning guarantee at all via this mechanism.
//
// Any other platform: a no-op that returns false.
//
// Returns false wherever pinning isn't actually honored by the platform.
// This is never treated as an error by ThreadPerCoreScheduler — pinning is
// a cache-locality optimization, not a correctness requirement. The "a
// task never migrates once assigned" guarantee comes entirely from
// software queue separation (each logical core has its own private queue,
// and its worker thread only ever looks at that queue), not from whether
// the OS actually keeps that worker thread's execution pinned to one
// physical core.
bool pin_to_core(std::thread::native_handle_type handle, unsigned core_index);

} // namespace wss::affinity
