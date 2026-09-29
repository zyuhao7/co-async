// 关键路径：定时器不早醒，并且会累加
//
// 定时器可以晚（本机负载高时必然晚），但不能早——早醒意味着时长换算
// （durationToKernelTimespec）或休眠循环算错了。所以只钉下界，上界放得很宽，
// 免得把调度抖动当成回归。
//
// 跑法：cmake --build build-dbg --target test_timer -j4 && ./build-dbg/test_timer
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;
using namespace std::literals;

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[test_timer] FAIL: %s\n", what);
        std::abort();
    }
}

static std::chrono::milliseconds elapsedSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t);
}

static Task<> amain() {
    {
        auto t0 = std::chrono::steady_clock::now();
        auto res = co_await co_sleep(80ms);
        auto ms = elapsedSince(t0);
        require(static_cast<bool>(res), "co_sleep reports success");
        require(ms >= 70ms && ms <= 2s, "co_sleep(80ms) should take about 80ms");
    }

    {
        // 三段串起来，下界是相加而不是取最大。
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 3; ++i) {
            (void)co_await co_sleep(30ms);
        }
        auto ms = elapsedSince(t0);
        require(ms >= 80ms, "three co_sleep(30ms) must add up");
    }

    {
        // 零时长不该把事件循环卡住。
        auto t0 = std::chrono::steady_clock::now();
        (void)co_await co_sleep(0ms);
        require(elapsedSince(t0) <= 500ms, "co_sleep(0ms) returns promptly");
    }

    std::fprintf(stderr, "[test_timer] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
