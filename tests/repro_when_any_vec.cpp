// 最小复现：when_any(vector) 的胜者会被败者覆盖 + 分配器类型不匹配
//
// when_any 的变参版在「取消判定之后」才记录 result/index，vector 版原来反了：
// 先跑完的那个调 cancel() 取消其余，被取消的那个醒来后仍会用自己的返回值把
// 胜者的结果盖掉——index 配败者的 value。
//
// 修复前：这个用例根本编译不过——vector 版写的是
//         std::vector<Task<>, Alloc>，而 Alloc 的 value_type 是调用方的
//         Task<int>，实例化即撞 vector 的
//         "std::vector must have the same value_type as its allocator"。
//         把 allocator 改对之后，行为上会看到 index 0 / value 非 1（败者覆盖）。
// 修复后：index 0、value 1（30ms 那个胜出）。
//
// 跑法：
//   cmake -B build-dbg -DCO_ASYNC_DEBUG=ON -DCO_ASYNC_INVALFIX=ON
//   cmake --build build-dbg --target repro_when_any_vec -j16
//   ./build-dbg/repro_when_any_vec   # 退出 0
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;
using namespace std::literals;

// 单次 co_await 只拿到 Expected<> 而丢弃错误，被取消的任务会照常返回自己的值；
// 这里显式查令牌，好让「败者被取消」在返回值上看得见（和 cancel_test 的
// compute() 一样）。
static Task<int> after(std::chrono::milliseconds d, int v) {
    CancelToken cancel = co_await co_cancel;
    auto res = co_await co_sleep(d);
    if (!res || cancel.is_canceled()) {
        co_return -1;
    }
    co_return v;
}

static Task<> amain() {
    std::vector<Task<int>> tasks;
    tasks.push_back(after(30ms, 1));
    tasks.push_back(after(60ms, 2));
    tasks.push_back(after(90ms, 3));

    auto res = co_await when_any(tasks);
    std::fprintf(stderr, "[probe] when_any -> index %zu value %d\n", res.index,
                 res.value);
    if (res.index != 0 || res.value != 1) {
        std::fprintf(stderr, "[probe] FAIL: expected index 0 value 1\n");
        std::abort();
    }
    std::fprintf(stderr, "[probe] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
