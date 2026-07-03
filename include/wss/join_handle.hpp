#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "wss/result.hpp"

namespace wss {

namespace detail {

template <class T>
struct HandleShared {
    std::mutex mu;
    std::condition_variable cv;
    std::optional<Result<T>> result;
};

} // namespace detail

// The producer side of a JoinHandle, held internally by a scheduler so it
// can deliver a task's outcome once execution finishes.
template <class T>
class ResultSetter {
public:
    explicit ResultSetter(std::shared_ptr<detail::HandleShared<T>> shared) : shared_(std::move(shared)) {}

    void set(Result<T> result) {
        {
            std::lock_guard<std::mutex> lock(shared_->mu);
            shared_->result.emplace(std::move(result));
        }
        shared_->cv.notify_all();
    }

private:
    std::shared_ptr<detail::HandleShared<T>> shared_;
};

// A handle to a task submitted to a Scheduler. Dropping the handle without
// calling join() is fine — the task still runs to completion, its result is
// simply discarded.
template <class T>
class JoinHandle {
public:
    explicit JoinHandle(std::shared_ptr<detail::HandleShared<T>> shared) : shared_(std::move(shared)) {}

    // Blocks until the task completes, returning its result or the error it
    // raised.
    Result<T> join() {
        std::unique_lock<std::mutex> lock(shared_->mu);
        shared_->cv.wait(lock, [this] { return shared_->result.has_value(); });
        Result<T> out = std::move(*shared_->result);
        shared_->result.reset();
        return out;
    }

    bool is_finished() const {
        std::lock_guard<std::mutex> lock(shared_->mu);
        return shared_->result.has_value();
    }

private:
    std::shared_ptr<detail::HandleShared<T>> shared_;
};

template <class T>
std::pair<ResultSetter<T>, JoinHandle<T>> new_handle_pair() {
    auto shared = std::make_shared<detail::HandleShared<T>>();
    return {ResultSetter<T>(shared), JoinHandle<T>(shared)};
}

} // namespace wss
