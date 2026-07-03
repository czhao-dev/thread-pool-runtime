#pragma once

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace wss {

// The error delivered through a JoinHandle when a task's body threw instead
// of returning normally — the C++ analog of Rust's JoinError::Panicked.
struct JoinError {
    std::string message;
    std::exception_ptr original;
};

// Extracts a best-effort human-readable message from the exception currently
// being handled. Must be called from inside a `catch (...)` block.
inline std::string current_exception_message() {
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "non-standard exception";
    }
}

// A hand-written Result<T, JoinError>, mirroring Rust's Result<T, JoinError>
// directly rather than rethrowing from join(). Keeps join() non-throwing by
// default; rethrow_if_error() is the escape hatch for callers who'd rather
// propagate via exceptions.
template <class T>
class Result {
public:
    static Result ok(T value) { return Result(std::in_place_index<0>, std::move(value)); }
    static Result err(JoinError e) { return Result(std::in_place_index<1>, std::move(e)); }

    bool is_ok() const noexcept { return state_.index() == 0; }

    T& value() & { return std::get<0>(state_); }
    const T& value() const& { return std::get<0>(state_); }
    T take_value() { return std::move(std::get<0>(state_)); }

    const JoinError& error() const& { return std::get<1>(state_); }

    void rethrow_if_error() const {
        if (is_ok()) {
            return;
        }
        if (error().original) {
            std::rethrow_exception(error().original);
        }
        throw std::runtime_error(error().message);
    }

private:
    Result(std::in_place_index_t<0>, T value) : state_(std::in_place_index<0>, std::move(value)) {}
    Result(std::in_place_index_t<1>, JoinError e) : state_(std::in_place_index<1>, std::move(e)) {}

    std::variant<T, JoinError> state_;
};

// Explicit specialization for void-returning tasks: std::variant<void, ...>
// is ill-formed, so Result<void> can't share the primary template's layout.
template <>
class Result<void> {
public:
    static Result ok() { return Result(std::in_place_index<0>); }
    static Result err(JoinError e) { return Result(std::in_place_index<1>, std::move(e)); }

    bool is_ok() const noexcept { return state_.index() == 0; }

    void value() const {}

    const JoinError& error() const& { return std::get<1>(state_); }

    void rethrow_if_error() const {
        if (is_ok()) {
            return;
        }
        if (error().original) {
            std::rethrow_exception(error().original);
        }
        throw std::runtime_error(error().message);
    }

private:
    explicit Result(std::in_place_index_t<0>) : state_(std::in_place_index<0>) {}
    Result(std::in_place_index_t<1>, JoinError e) : state_(std::in_place_index<1>, std::move(e)) {}

    std::variant<std::monostate, JoinError> state_;
};

} // namespace wss
