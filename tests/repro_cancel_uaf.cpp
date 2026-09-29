// 最小复现：取消回调里释放取消源 -> 释放后使用（use-after-free）
//
// CancelSourceImpl::doCancel 先把自己摘链、再同步执行取消回调；取消回调是同步
// 的——co_sleep 的 canceller 直接 resume 被取消的协程。若取消源就建在那个被
// 唤醒的协程帧里（很自然的写法：谁 await 谁持有源），被唤醒的协程会一路跑完
// 并释放 impl，而 doCancel 的协程帧还持有 this，回头再访问 mCancellers 就是
// 释放后使用。
//
// 修复前：直接跑 SIGSEGV（rc=139），valgrind 报
//         Invalid read of size 8 at ListHead::doClear (ilist.hpp:111)
//         ← IntrusiveList<CancellerBase>::clear ← CancelSourceImpl::doCancel
//         被读的块正是 ~CancelSourceBase 释放的 CancelSourceImpl（24 字节）。
// 修复后：退出 0，valgrind 干净。
//
// 跑法：
//   cmake -B build-dbg -DCO_ASYNC_DEBUG=ON -DCO_ASYNC_INVALFIX=ON
//   cmake --build build-dbg --target repro_cancel_uaf -j16
//   valgrind --error-exitcode=99 ./build-dbg/repro_cancel_uaf   # 修复前 exit 99
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;
using namespace std::literals;

static Task<int> cancellable(std::chrono::milliseconds d, int v) {
    CancelToken cancel = co_await co_cancel;
    auto res = co_await co_sleep(d);
    if (!res || cancel.is_canceled()) {
        co_return -1;
    }
    co_return v;
}

static Task<> amain() {
    // 取消源是 amain 的局部变量：取消唤醒 amain 后，amain 返回会释放 src，
    // 而 src.cancel() 里的 doCancel 此刻还在别的协程里跑。
    CancelSource src;
    auto sleeper = co_cancel.bind(src.token(), cancellable(1000ms, 7));
    co_spawn(co_bind([&]() -> Task<> {
        co_await co_sleep(50ms);
        co_await src.cancel();
    }));

    auto r = co_await sleeper;
    if (r != -1) {
        std::fprintf(stderr, "[probe] expected canceled(-1), got %d\n", r);
        std::abort();
    }
    std::fprintf(stderr, "[probe] sleeper canceled as expected\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    std::fprintf(stderr, "[probe] survived\n");
    return 0;
}
