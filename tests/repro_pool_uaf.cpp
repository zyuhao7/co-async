// 最小复现：分离的生产者协程活过连接池 -> 释放后使用（use-after-free）
//
// HTTPConnection::request_streamed 用 co_spawn(pipe_bind(...)) 起了一个分离的
// 读 body 协程，闭包捕获了连接池里那个 HTTPConnection 的 this。它卡在 socket
// recv 上时连接池析构，随后服务端再发一段数据让那次 recv 完成，它就被唤醒去
// 读写已释放的缓冲区。
//
// 修复前：valgrind / ASan 报 invalid read+write，栈是
//         BorrowedStream::fillbuf ← PlatformIOContext::waitEventsFor，
//         目标内存由 amain 里连接池析构（~HTTPProtocolVersion11）释放。
//         直接跑不一定会 abort——abort 与否取决于分配器布局，别拿退出码当判据。
// 修复后：valgrind / ASan 干净，退出 0。
//
// 跑法（本机 5.15 必须带 INVALFIX，否则 socket 建不起来、走不到这里）：
//   cmake -B build-dbg -DCO_ASYNC_DEBUG=ON -DCO_ASYNC_INVALFIX=ON
//   cmake --build build-dbg --target repro_pool_uaf -j16
//   valgrind --error-exitcode=99 ./build-dbg/repro_pool_uaf   # 修复前 exit 99
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;
using namespace std::literals;

static constexpr std::string_view kServeAt = "http://127.0.0.1:19371";

// 分块响应：先发一块让客户端读起来，挂 1 秒（客户端的连接池就在这段时间里析构），
// 再发一块好让客户端那次挂住的 recv 完成。
static Task<Expected<>> serveOnce(SocketListener listener) {
    auto client = co_await co_await listener_accept(listener);
    std::cerr << "[server] accepted\n";
    (void)co_await socket_write(client,
                                "HTTP/1.1 200 OK\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "Content-Type: text/event-stream\r\n"
                                "\r\n"
                                "5\r\nhello\r\n"sv);
    std::cerr << "[server] chunk 1 sent\n";
    co_await co_sleep(1s);
    std::cerr << "[server] chunk 2 sent (client pool should be gone by now)\n";
    (void)co_await socket_write(client, "5\r\nworld\r\n"sv);
    co_await co_sleep(500ms);
    co_return {};
}

static Task<Expected<>> amain() {
    auto listener = co_await co_await listener_bind(
        co_await AddressResolver().host(kServeAt).resolve_one());
    co_spawn(serveOnce(std::move(listener)));

    {
        HTTPConnectionPool pool;
        auto conn = co_await co_await pool.connect(kServeAt);
        HTTPRequest req = {.method = "GET", .uri = URI::parse("/")};
        auto [res, body] = co_await co_await conn->request_streamed(req, {});
        (void)res;
        // 故意不读 body：生产者协程此刻正卡在 socket recv 上
        co_await co_sleep(400ms);
        std::cerr << "[client] destroying pool with producer still blocked\n";
    } // pool 和 body 在这里析构，但生产者协程还挂着

    co_await co_sleep(1500ms);
    std::cerr << "[client] survived\n";
    co_return {};
}

int main() {
    co_main(amain());
    return 0;
}
