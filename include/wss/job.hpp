#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace wss {

// A move-only, type-erased, single-call callable — the C++ analog of Rust's
// Box<dyn FnOnce() + Send + 'static>. Deliberately not std::function: that
// requires CopyConstructible captures, which would force move-only task
// state into shared_ptr wrappers for no real benefit. No small-buffer
// optimization in v1 — this keeps the per-task allocation profile
// comparable to the ported Job type it replaces.
class UniqueFunction {
public:
    UniqueFunction() noexcept = default;

    template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, UniqueFunction>>>
    UniqueFunction(F&& f) : model_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}

    UniqueFunction(UniqueFunction&&) noexcept = default;
    UniqueFunction& operator=(UniqueFunction&&) noexcept = default;
    UniqueFunction(const UniqueFunction&) = delete;
    UniqueFunction& operator=(const UniqueFunction&) = delete;
    ~UniqueFunction() = default;

    void operator()() { model_->call(); }

    explicit operator bool() const noexcept { return static_cast<bool>(model_); }

private:
    struct Concept {
        virtual void call() = 0;
        virtual ~Concept() = default;
    };

    template <class F>
    struct Model final : Concept {
        F f;
        explicit Model(F&& fn) : f(std::move(fn)) {}
        void call() override { std::move(f)(); }
    };

    std::unique_ptr<Concept> model_;
};

using Job = UniqueFunction;

} // namespace wss
