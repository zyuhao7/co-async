// 最小复现：URI::url_decode 不认小写十六进制转义
//
// fromHex 原来只处理 '0'-'9' 与 'A'-'F'，小写 'a'-'f' 落进 else 分支返回 0。
// 于是 "%2f" 静默解成 '\0' 而不是 '/'——不是报错，是错值，更难查。
// percent-encoding 习惯写大写，所以平时不暴露。
//
// 修复前：第二、三条断言失败（"%2f" 与 "a%20b%2fc"）。
// 修复后：全部通过。
//
// 跑法：
//   cmake -B build-dbg -DCO_ASYNC_DEBUG=ON -DCO_ASYNC_INVALFIX=ON
//   cmake --build build-dbg --target repro_uri_decode -j4
//   ./build-dbg/repro_uri_decode   # 退出 0
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[repro_uri_decode] FAIL: %s\n", what);
        std::abort();
    }
}

int main() {
    std::setlocale(LC_ALL, "");

    require(std::string_view(URI::url_decode("%2F")) == "/",
            "uppercase %2F decodes to '/'");
    require(std::string_view(URI::url_decode("%2f")) == "/",
            "lowercase %2f must decode to '/' too, not to NUL");
    require(std::string_view(URI::url_decode("a%20b%2fc")) == "a b/c",
            "mixed-case escapes inside a longer string");

    // 边界：不完整的转义原样透出，不越界、不吞字符
    require(std::string_view(URI::url_decode("abc%")) == "abc%",
            "a trailing bare '%' is passed through");
    require(std::string_view(URI::url_decode("abc%A")) == "abc%A",
            "a truncated escape is passed through");

    std::fprintf(stderr, "[repro_uri_decode] PASS\n");
    return 0;
}
