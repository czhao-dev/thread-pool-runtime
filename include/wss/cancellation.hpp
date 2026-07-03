#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace wss {

class CancellationContext;

// A cloneable, shareable cancellation flag. Copying a token does not create
// a new flag; all copies observe the same cancellation state.
//
// The scheduler never forcibly kills a worker thread mid-task — that would
// be unsound in the presence of locks and destructors. Cancellation is
// purely cooperative: a task polls CancellationContext::is_cancelled() and
// exits early on its own terms.
class CancellationToken {
public:
    CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() noexcept { flag_->store(true, std::memory_order_seq_cst); }
    bool is_cancelled() const noexcept { return flag_->load(std::memory_order_seq_cst); }

    CancellationContext context() const;

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// Passed into a task spawned via spawn_cancellable, letting the task body
// poll for cancellation requests.
class CancellationContext {
public:
    explicit CancellationContext(CancellationToken token) : token_(std::move(token)) {}
    bool is_cancelled() const noexcept { return token_.is_cancelled(); }

private:
    CancellationToken token_;
};

inline CancellationContext CancellationToken::context() const { return CancellationContext(*this); }

} // namespace wss
