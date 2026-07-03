#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace wss {

// A single-owner-push/pop, multi-stealer work-stealing deque, implementing
// the algorithm from Lê, Pop, Cohen & Nardell, "Correct and Efficient
// Work-Stealing for Weak Memory Models" (PPoPP 2013) — the same family of
// algorithm crossbeam-deque (and this project's original Rust
// implementation) is built on.
//
// T must be default-constructible and movable.
//
// Slot representation: each slot stores an owning `T*` (heap-boxed), not a
// `T` by value. The original algorithm (and crossbeam-deque) stores T
// in-place and relies on the fact that a losing thread's non-atomic read of
// a contested slot is a "benign" data race, formally justified in Rust's
// memory model via careful unsafe reasoning. Reproducing that argument
// soundly in C++ for a non-trivially-copyable T (our Job is a move-only,
// heap-owning type) would require the same level of scrutiny. Boxing each
// element instead means every slot access is a well-defined atomic pointer
// load/store — trading one extra heap allocation per task for the
// guarantee that no slot access can ever be undefined behavior. This is a
// deliberate simplification, documented here and in the README, appropriate
// for a from-scratch implementation meant to be read and trusted.
//
// Memory reclamation: growing allocates a new, larger backing buffer and
// publishes it, but the OLD buffer is never freed while the deque is alive
// — only at destruction. This is sound because a Stealer can only ever be
// used while its owning deque (and the worker thread that owns it) is
// still alive; workers are joined, and their deques destroyed, only after
// shutdown, by which point no thread can hold a reference into this deque.
// This sidesteps the classic Chase-Lev reclamation hazard without hazard
// pointers or a hand-rolled epoch scheme. The cost is bounded-but-retained
// memory: a deque that grows from 256 to 1M entries retains ~12 buffers
// total (capacity doubles each time), not one per steal.
template <class T>
class ChaseLevDeque {
public:
    explicit ChaseLevDeque(std::size_t initial_capacity = 256) : top_(0), bottom_(0) {
        auto buf = std::make_unique<Buffer>(round_up_pow2(initial_capacity));
        buffer_.store(buf.get(), std::memory_order_relaxed);
        retired_.push_back(std::move(buf));
    }

    ChaseLevDeque(const ChaseLevDeque&) = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;

    ~ChaseLevDeque() {
        // Free any items still sitting in the deque (e.g. destroyed before
        // fully drained). The buffer arrays themselves are owned by
        // retired_ and freed automatically.
        Buffer* buf = buffer_.load(std::memory_order_relaxed);
        std::int64_t t = top_.load(std::memory_order_relaxed);
        std::int64_t b = bottom_.load(std::memory_order_relaxed);
        for (std::int64_t i = t; i < b; ++i) {
            delete buf->slots[buf->index(i)].load(std::memory_order_relaxed);
        }
    }

    // Owner-thread only.
    void push(T item) {
        std::int64_t b = bottom_.load(std::memory_order_relaxed);
        std::int64_t t = top_.load(std::memory_order_acquire);
        Buffer* buf = buffer_.load(std::memory_order_relaxed);

        if (b - t > static_cast<std::int64_t>(buf->capacity) - 1) {
            buf = grow(buf, t, b);
        }

        // Release here (not a raw fence) so a stealer's acquire load of this
        // exact slot has a direct, TSan-provable synchronizes-with edge to
        // the just-completed construction of *boxed. The seq_cst fences in
        // pop()/steal() are a separate concern (preventing simultaneous
        // claims of the same index) and do not by themselves make the
        // pointee's data visible.
        T* boxed = new T(std::move(item));
        buf->slots[buf->index(b)].store(boxed, std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    // Owner-thread only.
    std::optional<T> pop() {
        std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        Buffer* buf = buffer_.load(std::memory_order_relaxed);
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_relaxed);

        if (t > b) {
            // Was already empty; restore bottom and bail.
            bottom_.store(b + 1, std::memory_order_relaxed);
            return std::nullopt;
        }

        T* ptr = buf->slots[buf->index(b)].load(std::memory_order_relaxed);
        if (t == b) {
            // Last element: race any concurrent stealer for it.
            std::int64_t expected = t;
            bottom_.store(b + 1, std::memory_order_relaxed);
            if (!top_.compare_exchange_strong(expected, t + 1, std::memory_order_seq_cst,
                                               std::memory_order_relaxed)) {
                // Lost the race — the winning stealer owns `ptr` now.
                return std::nullopt;
            }
        }
        std::optional<T> out(std::move(*ptr));
        delete ptr;
        return out;
    }

    // Any thread, including the owner's peers.
    std::optional<T> steal() {
        std::int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t b = bottom_.load(std::memory_order_acquire);
        if (t >= b) {
            return std::nullopt;
        }

        Buffer* buf = buffer_.load(std::memory_order_acquire);
        T* ptr = buf->slots[buf->index(t)].load(std::memory_order_acquire);

        std::int64_t expected = t;
        if (!top_.compare_exchange_strong(expected, t + 1, std::memory_order_seq_cst,
                                           std::memory_order_relaxed)) {
            // Lost the race — do not touch `ptr`; the winner owns it.
            return std::nullopt;
        }
        if (ptr == nullptr) {
            return std::nullopt;
        }
        std::optional<T> out(std::move(*ptr));
        delete ptr;
        return out;
    }

private:
    struct Buffer {
        std::size_t capacity;
        std::size_t mask;
        std::vector<std::atomic<T*>> slots;

        explicit Buffer(std::size_t cap) : capacity(cap), mask(cap - 1), slots(cap) {}

        std::size_t index(std::int64_t i) const noexcept {
            return static_cast<std::size_t>(i) & mask;
        }
    };

    static std::size_t round_up_pow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) {
            p <<= 1;
        }
        return p;
    }

    // Owner-thread only (called from push()).
    Buffer* grow(Buffer* old_buf, std::int64_t t, std::int64_t b) {
        auto new_buf = std::make_unique<Buffer>(old_buf->capacity * 2);
        for (std::int64_t i = t; i != b; ++i) {
            T* ptr = old_buf->slots[old_buf->index(i)].load(std::memory_order_relaxed);
            new_buf->slots[new_buf->index(i)].store(ptr, std::memory_order_release);
        }
        Buffer* raw = new_buf.get();
        buffer_.store(raw, std::memory_order_release);
        retired_.push_back(std::move(new_buf));
        return raw;
    }

    alignas(64) std::atomic<std::int64_t> top_;
    alignas(64) std::atomic<std::int64_t> bottom_;
    std::atomic<Buffer*> buffer_;
    std::vector<std::unique_ptr<Buffer>> retired_; // owner-thread only; freed at destruction
};

} // namespace wss
