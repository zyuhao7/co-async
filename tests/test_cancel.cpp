// 关键路径：取消必须真的落到 io_uring 请求上，而不只是把取消令牌置位
//
// cancelGuard 曾经写死 IORING_ASYNC_CANCEL_ALL，本机 5.15 内核对这个 key 直接
// 回 -EINVAL：取消请求自己失败，目标 op 照旧挂着。于是所有带 CancelToken 的
// fs_/socket_ 调用点被取消后都不会返回，调用方按取消逻辑该走的分支走不到。
//
// 修复前：第一步就挂住（CTest 超时）——pipe 上没有数据，fs_read 的 io_uring
//         请求没人取消。
// 修复后：fs_read 返回 operation_canceled；紧接着第二次读还能拿到数据，说明
//         取消的只是那一个请求，环本身没坏。
//
// 跑法：cmake --build build-dbg --target test_cancel -j4 && ./build-dbg/test_cancel
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>
#include <unistd.h>

using namespace co_async;
using namespace std::literals;

static Task<Expected<std::size_t>> readOnce(FileHandle &fd) {
    std::array<char, 16> buf;
    co_return co_await fs_read(fd, buf, co_await co_cancel);
}

// 取消令牌由父协程经 await_transform(Task) 传下来，子协程里应当看得见。
static Task<bool> seesCancelled() {
    CancelToken cancel = co_await co_cancel;
    (void)co_await co_sleep(1s);
    co_return cancel.is_canceled();
}

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[test_cancel] FAIL: %s\n", what);
        std::abort();
    }
}

static Task<> amain() {
    int p[2];
    require(pipe2(p, O_CLOEXEC) == 0, "pipe2");
    FileHandle rd(p[0]), wr(p[1]);

    {
        CancelSource src;
        auto reading = co_cancel.bind(src.token(), readOnce(rd));
        co_spawn(co_bind([&]() -> Task<> {
            (void)co_await co_sleep(50ms);
            co_await src.cancel();
        }));
        auto res = co_await reading;
        require(!res, "cancelled read should not succeed");
        require(res.error() == std::errc::operation_canceled,
                "cancelled read should report operation_canceled");
    }

    // 环继续可用：写一个字节，不带取消令牌的读应当原样拿到它。
    require(write(p[1], "x", 1) == 1, "write");
    std::array<char, 4> buf{};
    auto n = co_await fs_read(rd, buf);
    require(static_cast<bool>(n) && *n == 1 && buf[0] == 'x',
            "ring should still serve reads after a cancel");

    {
        CancelSource src;
        auto sleeper = co_cancel.bind(src.token(), seesCancelled());
        co_spawn(co_bind([&]() -> Task<> {
            (void)co_await co_sleep(50ms);
            co_await src.cancel();
        }));
        require(co_await sleeper, "child coroutine should see the cancel token");
    }

    std::fprintf(stderr, "[test_cancel] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
