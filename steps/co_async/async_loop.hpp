#pragma once

#include "timer_loop.hpp"
#include "epoll_loop.hpp"
#include <thread>

namespace co_async {

struct AsyncLoop {
    // 返回 true 表示这一趟至少干了一点活（跑过 epoll 或睡过定时器），
    // run_task 靠它一边推一边判断是否已经“无事可做”
    bool run() {
        bool busy = false;
        while (true) {
            auto timeout = mTimerLoop.run();
            if (mEpollLoop.hasEvent()) {
                mEpollLoop.run(timeout);
                busy = true;
            } else if (timeout) {
                std::this_thread::sleep_for(*timeout);
                busy = true;
            } else {
                break;
            }
        }
        return busy;
    }

    operator TimerLoop &() {
        return mTimerLoop;
    }

    operator EpollLoop &() {
        return mEpollLoop;
    }

private:
    TimerLoop mTimerLoop;
    EpollLoop mEpollLoop;
};

} // namespace co_async
