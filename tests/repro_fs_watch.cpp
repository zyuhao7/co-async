// 最小复现：FileWatch::wait() 把取消/读错误 throw 出来，而不是返回 Expected 错误
//
// 原实现 `if (!co_await mStream.getstruct(...)) throw std::runtime_error(...)`：
// 这是一个 Task<Expected<WaitFileResult>>，契约是回错误码，却用了 throw。被取消
// 时那次读回 operation_canceled，会被伪装成「EOF while reading struct」抛出去；
// 调用方按 Expected 契约不会去接，于是 terminate/abort（O_DIRECT 配置下实测
// abort 134）。同理 mWatches.at() 对已删/已移走的 wd 抛 out_of_range。
//
// 修复前：取消这条路径 abort（或至少抛出未捕获异常）。
// 修复后：取消回 operation_canceled，正常写入事件照常拿到。
//
// 跑法：
//   cmake -B build-dbg -DCO_ASYNC_DEBUG=ON -DCO_ASYNC_INVALFIX=ON
//   cmake --build build-dbg --target repro_fs_watch -j16
//   ./build-dbg/repro_fs_watch   # 退出 0
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>
#include <fstream>

using namespace co_async;
using namespace std::literals;

static Task<> amain() {
    auto dir = std::filesystem::temp_directory_path() / "co_async_fs_watch_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // 1) 正常路径：另一个协程写完文件后应拿到 OnWriteFinished 事件
    {
        FileWatch w;
        w.watch(dir, FileWatch::OnWriteFinished);
        co_spawn(co_bind([dir]() -> Task<> {
            co_await co_sleep(50ms);
            std::ofstream(dir / "hello.txt") << "hi";
            co_return;
        }));
        auto res = co_await w.wait();
        if (!res) {
            std::fprintf(stderr, "[probe] FAIL: wait() errored: %s\n",
                         res.error().message().c_str());
            std::abort();
        }
        std::fprintf(stderr, "[probe] got event for %s\n",
                     res->path.string().c_str());
        if (res->path.filename() != "hello.txt") {
            std::fprintf(stderr, "[probe] FAIL: unexpected path\n");
            std::abort();
        }
    }

    // 2) 取消路径：不能 throw，要回 operation_canceled
    {
        CancelSource src;
        FileWatch w;
        w.watch(dir, FileWatch::OnWriteFinished);
        auto bounded = co_cancel.bind(
            src.token(),
            co_bind([&w]() -> Task<Expected<FileWatch::WaitFileResult>> {
                co_return co_await w.wait();
            }));
        co_spawn(co_bind([&src]() -> Task<> {
            co_await co_sleep(50ms);
            co_await src.cancel();
        }));
        auto res = co_await bounded;
        if (res) {
            std::fprintf(stderr, "[probe] FAIL: expected cancellation\n");
            std::abort();
        }
        std::fprintf(stderr, "[probe] cancel returned error: %s\n",
                     res.error().message().c_str());
    }

    std::filesystem::remove_all(dir);
    std::fprintf(stderr, "[probe] survived\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
