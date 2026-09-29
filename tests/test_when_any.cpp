// 关键路径：when_any 谁先完成谁胜出，以及 co_timeout 的超时语义
//
// 变参 when_any 返回 variant，胜出者即 variant 的 index；when_any_common 另给
// 一个 index；co_timeout 是 when_any(任务, co_sleep(超时)) 的封装，超时分支
// 回 stream_timeout。
//
// 被取消的败者必须不能覆盖胜者——vector 版曾在这里出错（见
// tests/repro_when_any_vec.cpp），变参版也一并钉住。
//
// 跑法：cmake --build build-dbg --target test_when_any -j4 && ./build-dbg/test_when_any
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;
using namespace std::literals;

static Task<int> after(std::chrono::milliseconds d, int v) {
    CancelToken cancel = co_await co_cancel;
    auto res = co_await co_sleep(d);
    if (!res || cancel.is_canceled()) {
        co_return -1;
    }
    co_return v;
}

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[test_when_any] FAIL: %s\n", what);
        std::abort();
    }
}

static Task<> amain() {
    {
        auto res = co_await when_any(after(20ms, 1), after(60ms, 2), after(90ms, 3));
        require(res.index() == 0, "fastest task should be the winner");
        require(std::get<0>(res) == 1, "winner keeps its own value");
    }

    {
        auto res = co_await when_any_common(after(20ms, 11), after(60ms, 22));
        require(res.index == 0 && res.value == 11,
                "when_any_common reports the winner's index and value");
    }

    {
        auto fast = co_await co_timeout(after(20ms, 7), 500ms);
        require(fast && *fast == 7, "task faster than the timeout returns its value");
        auto slow = co_await co_timeout(after(500ms, 9), 30ms);
        require(!slow && slow.error() == std::errc::stream_timeout,
                "task slower than the timeout returns stream_timeout");
    }

    std::fprintf(stderr, "[test_when_any] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
