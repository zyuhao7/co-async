// 最小复现：BorrowedStream::write 的分支算长度用的是输入缓冲
//
// stream_base.hpp:495-502 的 write(span)：
//
//     if (!buffull()) {
//         auto n = std::min(mInBuffer.size() - mInIndex, buffer.size());
//         co_await co_await putspan(buffer.subspan(0, n));
//         co_return n;
//     }
//
// 这里在「输出缓冲没满、直接进缓冲」的分支里算可用空间，量的是 mInBuffer
// （输入），但缓冲的是 mOutBuffer（输出）——同一段代码里的 putspan/trywrite
// 用的都是 mOutBuffer.size() - mOutIndex（:353、:379），这一行是孤例。
//
// 后果不是写错字节而是**返回 0 且什么都不写**：只要 mInBuffer 被读空到末尾
// （mInIndex == mInBuffer.size()）而输出缓冲已分配且没满，n 就是 0，调用方
// 拿到「写了 0 字节」。所以本用例不用赌 8192 的默认缓冲：显式 allocinbuf(16)
// 把输入缓冲压小，让「读满 16 字节」就等于「读空到末尾」，触发点确定。
//
// 修复前：write 返回 0，第二条断言失败。
// 修复后：write 返回 buffer.size()，文件内容也逐字节对上。
//
// 跑法：
//   cmake -B build-dbg -DCO_ASYNC_DEBUG=ON -DCO_ASYNC_INVALFIX=ON
//   cmake --build build-dbg --target repro_stream_write -j4
//   ./build-dbg/tests/repro_stream_write   # 退出 0
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[repro_stream_write] FAIL: %s\n", what);
        std::abort();
    }
}

static Task<> amain() {
    auto path = std::filesystem::temp_directory_path() /
                "co_async_repro_stream_write.bin";

    String seed;
    for (int i = 0; i < 16; ++i) {
        seed.push_back(static_cast<char>('a' + i));
    }
    require(static_cast<bool>(
                co_await file_write(path, std::string_view(seed))),
            "seed file written");

    auto streamOpt = co_await file_open(path, OpenMode::ReadWrite);
    require(streamOpt.has_value(), "open the file read-write");
    auto &stream = *streamOpt;

    // 输入缓冲压到 16 字节：下面 getn(16) 一读就把 mInBuffer 读满/读空到末尾。
    stream.allocinbuf(16);

    String got;
    require(static_cast<bool>(co_await stream.getn(got, 16)),
            "getn 16 bytes");
    require(got == seed, "the seed bytes read back intact");

    // 让输出缓冲已分配且没满——就是 write 想走的那个分支的前提。
    require(static_cast<bool>(co_await stream.putchar('X')),
            "buffered putchar");

    String tail;
    for (int i = 0; i < 16; ++i) {
        tail.push_back(static_cast<char>('A' + i));
    }
    auto n = co_await stream.write(
        std::span<char const>(tail.data(), tail.size()));
    require(n.has_value(), "write reports a result");
    require(*n == tail.size(),
            "write must buffer the whole span, not report 0 bytes");

    require(static_cast<bool>(co_await stream.flush()), "flush");
    co_await stream.close();

    auto all = co_await file_read(path);
    require(all.has_value(), "read the file back");
    String expected = seed;
    expected.push_back('X');
    expected += tail;
    require(*all == expected,
            "the buffered write must land in the file, in order");

    std::fprintf(stderr, "[repro_stream_write] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
