// 最小复现：HTTPProtocolVersion11::readEncoded 的 Deflate 分支
//
// 原来这条分支挂了三个子任务，其中两个都在读同一个 socket：
//
//     pipe_bind(std::move(w), &HTTPProtocolVersion11::readChunked, this)
//     co_bind([this, w = std::move(w)]{ ... readChunked(w); ... })
//
// pipe_bind(stream, func, args...) 的语义是 std::invoke(func, args..., stream)
// （iostream/pipe_stream.hpp:13-25），所以第一个就是 this->readChunked(w)——
// 与第二个完全重复，且同一个 w 被 std::move 两次。第三个子任务写的是
// zlib_deflate(r, body)：解压路径上做了压缩。对照写侧 Deflate 与读侧 Gzip，
// 正确形态都是两个子任务。
//
// 本用例不依赖 CO_ASYNC_ZLIB 也能钉住「重复读」那一半：在 body 之后放一段
// TRAILER，读完之后 socket 的缓冲里应当原封不动地剩下它。重复读会把 TRAILER
// 的前若干字节也当 body 吃掉。
//
// 修复前：留下的不是 TRAILER（重复读把它吃掉一部分）。
// 修复后：留下 TRAILER；CO_ASYNC_ZLIB 打开时再加跑一条真正解压的断言。
//
// 跑法：
//   cmake -B build-dbg -DCO_ASYNC_DEBUG=ON -DCO_ASYNC_INVALFIX=ON
//   cmake --build build-dbg --target repro_http_deflate -j4
//   ./build-dbg/repro_http_deflate   # 退出 0
//   （想连解压一起验：configure 时加 -DCO_ASYNC_ZLIB=ON）
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;
using namespace std::literals;

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[repro_http_deflate] FAIL: %s\n", what);
        std::abort();
    }
}

static std::unique_ptr<HTTPProtocolVersion11>
protocolOver(std::string_view &wire) {
    return std::make_unique<HTTPProtocolVersion11>(
        make_stream<IStringStream>(wire));
}

static Task<> amain() {
    {
        // content-length 的 Deflate 响应，body 是 4 个字节，后面跟一段 TRAILER。
        std::string_view wire =
            "HTTP/1.1 200 OK\r\n"
            "content-encoding: deflate\r\n"
            "content-length: 4\r\n"
            "\r\n"
            "ABCDTRAILER"sv;
        auto proto = protocolOver(wire);

        HTTPResponse res;
        auto head = co_await proto->readResponse(res);
        require(head && res.status == 200, "the response head parses");

        // zlib 关着时这里返回 function_not_supported——本条不测解压，只测
        // 「socket 被读了几次」，所以不检查它的成败。
        String body;
        (void)co_await proto->readBody(body);

        auto left = proto->sock.peekbuf();
        require(std::string_view(left.data(), left.size()) == "TRAILER",
                "the deflate body must be pulled off the socket exactly once");
    }

#if CO_ASYNC_ZLIB
    {
        // 真正解压：把一段明文压成 deflate，再走一遍解码路径，值必须回来。
        std::string_view plain = "hello, deflate world"sv;
        String compressed;
        {
            auto is = make_stream<IStringStream>(plain);
            auto os = make_stream<OStringStream>(compressed);
            auto e = co_await zlib_deflate(is, os);
            require(e.has_value(), "zlib_deflate on the test input");
            co_await co_await os.flush();
        }

        String wire = "HTTP/1.1 200 OK\r\ncontent-encoding: deflate\r\n"
                      "content-length: " +
                      to_string(compressed.size()) + "\r\n\r\n" + compressed;
        auto proto = protocolOver(std::string_view(wire));

        HTTPResponse res;
        auto head = co_await proto->readResponse(res);
        require(head && res.status == 200, "the compressed response head parses");

        String body;
        auto got = co_await proto->readBody(body);
        require(got.has_value(),
                "a deflate body must decode, not come back as an error");
        require(std::string_view(body) == plain,
                "the decoded body must equal the original plaintext");
    }
#endif

    std::fprintf(stderr, "[repro_http_deflate] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
