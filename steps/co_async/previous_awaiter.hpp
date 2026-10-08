#pragma once

#include <coroutine>

namespace co_async {

struct PreviousAwaiter {
    std::coroutine_handle<> mPrevious;
    // detach 的任务没有上一级在等它：跑完就地销毁，别把帧漏在那儿
    bool mDetached = false;

    bool await_ready() const noexcept {
        return mDetached;
    }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> coroutine) const noexcept {
        return mPrevious;
    }

    void await_resume() const noexcept {}
};

} // namespace co_async
