#pragma once

#include "debug.hpp"
#include <exception>
#include <coroutine>
#include <utility>
#include "uninitialized.hpp"
#include "previous_awaiter.hpp"

namespace co_async {

template <class T>
struct Promise {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(mPrevious, mDetached);
    }

    void unhandled_exception() noexcept {
        mException = std::current_exception();
    }

    void return_value(T &&ret) {
        mResult.putValue(std::move(ret));
    }

    void return_value(T const &ret) {
        mResult.putValue(ret);
    }

    T result() {
        if (mException) [[unlikely]] {
            std::rethrow_exception(mException);
        }
        return mResult.moveValue();
    }

    auto get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    std::coroutine_handle<> mPrevious;
    std::exception_ptr mException{};
    bool mDetached = false; // true：无人 await，final_suspend 处自动销毁帧
    Uninitialized<T> mResult; // destructed??

    Promise &operator=(Promise &&) = delete;
};

template <>
struct Promise<void> {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(mPrevious, mDetached);
    }

    void unhandled_exception() noexcept {
        mException = std::current_exception();
    }

    void return_void() noexcept {}

    void result() {
        if (mException) [[unlikely]] {
            std::rethrow_exception(mException);
        }
    }

    auto get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    std::coroutine_handle<> mPrevious;
    std::exception_ptr mException{};
    bool mDetached = false; // true：无人 await，final_suspend 处自动销毁帧

    Promise &operator=(Promise &&) = delete;
};

template <class T = void, class P = Promise<T>>
struct [[nodiscard]] Task {
    using promise_type = P;

    Task(std::coroutine_handle<promise_type> coroutine = nullptr) noexcept
        : mCoroutine(coroutine) {}

    Task(Task &&that) noexcept : mCoroutine(that.mCoroutine) {
        that.mCoroutine = nullptr;
    }

    Task &operator=(Task &&that) noexcept {
        std::swap(mCoroutine, that.mCoroutine);
    }

    ~Task() {
        if (mCoroutine)
            mCoroutine.destroy();
    }

    struct Awaiter {
        bool await_ready() const noexcept {
            return false;
        }

        std::coroutine_handle<promise_type>
        await_suspend(std::coroutine_handle<> coroutine) const noexcept {
            promise_type &promise = mCoroutine.promise();
            promise.mPrevious = coroutine;
            return mCoroutine;
        }

        T await_resume() const {
            return mCoroutine.promise().result();
        }

        std::coroutine_handle<promise_type> mCoroutine;
    };

    auto operator co_await() const noexcept {
        return Awaiter(mCoroutine);
    }

    operator std::coroutine_handle<promise_type>() const noexcept {
        return mCoroutine;
    }

    // 交出协程帧的所有权，之后本 Task 析构也不会销毁它
    std::coroutine_handle<promise_type> release() noexcept {
        auto coroutine = mCoroutine;
        mCoroutine = nullptr;
        return coroutine;
    }

private:
    std::coroutine_handle<promise_type> mCoroutine;
};

template <class Loop, class T, class P>
T run_task(Loop &loop, Task<T, P> const &t) {
    auto a = t.operator co_await();
    a.await_suspend(std::noop_coroutine()).resume();
    while (loop.run())
        ;
    return a.await_resume();
}

template <class T, class P>
void spawn_task(Task<T, P> task) {
    auto coroutine = task.operator co_await().await_suspend(
        std::noop_coroutine());
    // 没人 await 这个任务：打成 detach，让它在 final_suspend 处自己销毁
    coroutine.promise().mDetached = true;
    // 交出所有权：否则函数返回时这个按值传入的 task 会把正在跑的帧析构掉
    task.release();
    coroutine.resume();
}

} // namespace co_async
