#include "co_async/debug.hpp"
#include "co_async/task.hpp"
#include "co_async/timer_loop.hpp"
#include "co_async/epoll_loop.hpp"
#include "co_async/when_any.hpp"
#include "co_async/when_all.hpp"
#include "co_async/and_then.hpp"
#include <thread>
#include <fcntl.h>
#include <unistd.h>

using namespace std::chrono_literals;

co_async::EpollLoop epollLoop;
co_async::TimerLoop timerLoop;

co_async::Task<std::string> read_string(co_async::AsyncFile &file) {
    co_await wait_file_event(epollLoop, file, EPOLLIN | EPOLLRDHUP);
    std::string s;
    size_t chunk = 8;
    while (true) {
        std::size_t exist = s.size();
        s.resize(exist + chunk);
        std::span<char> buffer(s.data() + exist, chunk);
        auto len = co_await read_file(epollLoop, file, buffer);
        if (len != chunk) {
            s.resize(exist + len);
            break;
        }
        if (chunk < 65536)
            chunk *= 4;
    }
    co_return s;
}

co_async::Task<void> async_main() {
    co_async::AsyncFile file(STDIN_FILENO);
    file.setNonblock(); // read_file 内部是非阻塞读，fd 必须先置为非阻塞
    while (true) {
        auto s = co_await read_string(file);
        if (s.empty()) break; // EOF：对端关闭
        debug(), "读到了", s;
        if (s == "quit\n")
            break;
    }
}

int main() {
    auto t = async_main();
    // 顶层任务没人 co_await 它，final_suspend 需要一个落脚点，否则 resume 空句柄崩溃
    std::coroutine_handle<co_async::Promise<void>> h = t;
    h.promise().mPrevious = std::noop_coroutine();
    h.resume();
    while (!h.done()) {
        auto timeout = timerLoop.run();
        epollLoop.run(timeout);
    }
    return 0;
}
