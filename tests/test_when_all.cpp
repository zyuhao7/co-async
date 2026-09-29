// 关键路径：when_all 的并发语义与结果收集
//
// 两个要求：所有子任务在任何一个完成之前就已经启动（真并发，不是排队），
// 结果按传入顺序回到 tuple / vector 里。
//
// 判据不用墙钟（本机负载高时会抖），用「同时在跑的任务数的峰值」——所有子
// 任务都先 ++ 再 co_sleep，只要它们真的一起启动，峰值必然等于任务数。
//
// 跑法：cmake --build build-dbg --target test_when_all -j4 && ./build-dbg/test_when_all
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;
using namespace std::literals;

static int gRunning = 0;
static int gPeak = 0;

static Task<int> work(std::chrono::milliseconds d, int v) {
    ++gRunning;
    gPeak = std::max(gPeak, gRunning);
    auto res = co_await co_sleep(d);
    --gRunning;
    co_return res ? v : -1;
}

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[test_when_all] FAIL: %s\n", what);
        std::abort();
    }
}

static Task<> amain() {
    {
        gPeak = 0;
        auto [a, b, c] =
            co_await when_all(work(20ms, 1), work(30ms, 2), work(40ms, 3));
        require(a == 1 && b == 2 && c == 3, "tuple values follow argument order");
        require(gPeak == 3, "all three should be in flight at once");
    }

    {
        gPeak = 0;
        std::vector<Task<int>> tasks;
        tasks.push_back(work(20ms, 10));
        tasks.push_back(work(30ms, 20));
        auto res = co_await when_all(tasks);
        require(res.size() == 2 && res[0] == 10 && res[1] == 20,
                "vector values follow vector order");
        require(gPeak == 2, "both vector tasks should be in flight at once");
    }

    std::fprintf(stderr, "[test_when_all] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
