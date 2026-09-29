// 阶段 5 第 182 项：验证 README 的两条性能宣称
//   - 「纳秒级定时器」：定时器按 steady_clock 的纳秒精度排序、按时唤醒
//   - 「百万级并发」：同一条事件循环上挂起几十万/上百万协程仍能全部完成
//
// 不是基准测试，是**正确性**测试：只断言「顺序对、都完成」，不判吞吐。
// 吞吐/绝对量级受本机（WSL2、内存/swap 紧张）约束，仅打印参考，不作判据。
//
// 规模由 argv[1] 给（默认 20000，CTest 用这个；手工跑百万级直接传 1000000）。
//   手工：./build-dbg/tests/test_scale 1000000
//   CTest：ctest --test-dir build-dbg -R unit/test_scale
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>
#include <sys/resource.h>

using namespace co_async;
using namespace std::literals;

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[test_scale] FAIL: %s\n", what);
        std::abort();
    }
}

static std::size_t peakRssKB() {
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<std::size_t>(ru.ru_maxrss);
}

static std::vector<std::size_t> gOrder;
static std::size_t gDone = 0;

// 延时后按完成顺序登记（单线程事件循环，无竞态）
static Task<> orderedSleeper(std::size_t idx,
                             std::chrono::steady_clock::duration d) {
    co_await co_sleep(d);
    gOrder.push_back(idx);
    co_return;
}

static Task<> counter(std::chrono::steady_clock::duration d) {
    co_await co_sleep(d);
    ++gDone;
    co_return;
}

static Task<> amain(std::size_t n) {
    auto const t0 = std::chrono::steady_clock::now();

    // ---- A. 纳秒级定时器：亚毫秒间隔的唤醒顺序必须严格单调 ----
    // 64 个睡者，延时 1ms + idx*1us（1us 间隔，全部落在同一毫秒内）。
    // 若定时器被量化到毫秒、或红黑树比较退化成粗粒度，这里就会乱序。
    constexpr std::size_t kSub = 64;
    for (std::size_t i = 0; i < kSub; ++i) {
        co_spawn(orderedSleeper(i, 1ms + std::chrono::microseconds(i)));
    }
    while (gOrder.size() < kSub) {
        co_await co_sleep(50us);
    }
    require(gOrder.size() == kSub, "all sub-millisecond sleepers ran");
    for (std::size_t i = 0; i < kSub; ++i) {
        require(gOrder[i] == i,
                "sub-millisecond deadlines wake in deadline order, not in a "
                "millisecond bucket");
    }
    auto const subUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();

    // co_sleep(0) 的单次往返延迟（ns 量级）：证明定时器路径本身不在毫秒档
    auto const q0 = std::chrono::steady_clock::now();
    co_await co_sleep(0ns);
    auto const zeroNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - q0)
                            .count();

    std::fprintf(stderr,
                 "[test_scale] timers: %zu sub-ms deadlines in %lldus; "
                 "co_sleep(0) round-trip %lldns\n",
                 kSub, static_cast<long long>(subUs),
                 static_cast<long long>(zeroNs));

    // ---- B. 百万级并发：N 个协程同时挂在一条事件循环上，全部完成 ----
    auto const rssBefore = peakRssKB();
    auto const b0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        co_spawn(counter(2ms));
    }
    while (gDone < n) {
        co_await co_sleep(500us);
    }
    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - b0)
                        .count();
    auto const rssAfter = peakRssKB();

    require(gDone == n, "every concurrently-spawned coroutine completed");
    std::fprintf(stderr,
                 "[test_scale] concurrency: %zu coroutines in %lldms; "
                 "peak RSS %zuMB (baseline %zuMB, +%zuMB)\n",
                 n, static_cast<long long>(ms), rssAfter / 1024,
                 rssBefore / 1024, (rssAfter - rssBefore) / 1024);

    std::fprintf(stderr, "[test_scale] PASS\n");
    co_return;
}

int main(int argc, char **argv) {
    std::setlocale(LC_ALL, "");
    std::size_t n = 20000;
    if (argc > 1) {
        n = std::strtoull(argv[1], nullptr, 10);
    }
    co_main(amain(n));
    return 0;
}
