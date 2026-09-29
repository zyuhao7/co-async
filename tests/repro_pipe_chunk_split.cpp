// 最小复现：IPipeStream::raw_read 在小 span 下丢掉整段 chunk 的余部
//
// pipe_stream.cpp:17-31 的读端每被调用一次就从队列 pop 出一整段 chunk，
// 然后只搬 min(buffer.size(), chunk.size())：
//
//     auto chunk = co_await co_await mPipe->mChunks.pop();
//     auto n = std::min(buffer.size(), chunk.size());
//     std::memcpy(buffer.data(), chunk.data(), n);
//     co_return n;
//
// 弹出即从队列移除，chunk 余部随局部变量销毁 —— 只有当调用方给的 span 比
// chunk 小时才暴露，而 BorrowedStream 的常规路径（fillbuf）永远给 8192，
// 所以平时看不见。但 write 侧一次 push 就是把整个 span 当一段 chunk
// （OPipeStream::raw_write），span 可以大于任何读取方要的量。
//
// 本用例把「读一半、再读」做成确定触发：先推进去 8 字节一段，用 4 字节的
// span 读两次，第二次必须读回**同一段**的后 4 字节，而不是下一段。
//
// 修复前：第二次读到 "BBBB"（第一段的后半被丢了）。
// 修复后：第二次读到 "AAAA"，顺序与写入一致。
//
// 跑法：
//   cmake -B build-dbg -DCO_ASYNC_DEBUG=ON -DCO_ASYNC_INVALFIX=ON
//   cmake --build build-dbg --target repro_pipe_chunk_split -j4
//   ./build-dbg/tests/repro_pipe_chunk_split   # 退出 0
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[repro_pipe_chunk_split] FAIL: %s\n", what);
        std::abort();
    }
}

static Task<> amain() {
    auto pipes = pipe_stream();
    auto &in = pipes[0];
    auto &out = pipes[1];

    // 新流的输出缓冲还没分配，buffull() 为真，所以 write 直接落到 raw_write，
    // 整个 span 成为一段 chunk（不经过 8192 的输出缓冲切分）。
    String first = "AAAAAAAA";
    auto w1 = co_await out.write(std::span<char const>(first.data(), first.size()));
    require(w1.has_value() && *w1 == first.size(), "push the first 8-byte chunk");

    char buf[4] = {};
    auto n1 = co_await in.read(std::span<char>(buf, 4));
    require(n1.has_value() && *n1 == 4, "read the first 4 bytes");
    require(std::string_view(buf, 4) == "AAAA", "first read sees the head");

    String second = "BBBBBBBB";
    auto w2 =
        co_await out.write(std::span<char const>(second.data(), second.size()));
    require(w2.has_value() && *w2 == second.size(), "push the second chunk");

    char buf2[4] = {};
    auto n2 = co_await in.read(std::span<char>(buf2, 4));
    require(n2.has_value() && *n2 == 4, "read 4 more bytes");
    require(std::string_view(buf2, 4) == "AAAA",
            "the rest of the first chunk must be served first, not dropped");

    char buf3[4] = {};
    auto n3 = co_await in.read(std::span<char>(buf3, 4));
    require(n3.has_value() && *n3 == 4, "read the second chunk's head");
    require(std::string_view(buf3, 4) == "BBBB",
            "then the second chunk, in order");

    std::fprintf(stderr, "[repro_pipe_chunk_split] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
