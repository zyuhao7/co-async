// 关键路径：Expected 的错误传递
//
// 本库让函数返回 Task<Expected<T>> 而不是抛异常，调用方用双重 co_await 接：
// 外层拿到 Expected，内层由 TaskPromise::await_transform(Expected) 接住，出错
// 就地 return，把错误原样变成当前协程的返回值。中间层不需要写任何 if。
// 这套机制要是断了，错误会被当成成功往下走（fs_watch 曾把取消当成
// 「EOF while reading」抛出去，见 tests/repro_fs_watch.cpp）。
//
// 跑法：cmake --build build-dbg --target test_expected -j4 && ./build-dbg/test_expected
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;
using namespace std::literals;

static Task<Expected<int>> failWith(std::errc e) {
    co_return e;
}

static Task<Expected<int>> succeed(int v) {
    co_return v;
}

// 中间层：成功就把值透出去，失败则由双重 co_await 直接返回错误。
static Task<Expected<int>> passThrough(std::errc e) {
    co_return co_await co_await failWith(e);
}

// 同一个双重 co_await 在成功路径上给出内层的值（int），不是 Expected。
static Task<Expected<int>> passThroughOk(int v) {
    int inner = co_await co_await succeed(v);
    co_return inner + 1;
}

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[test_expected] FAIL: %s\n", what);
        std::abort();
    }
}

static Task<> amain() {
    {
        auto res = co_await failWith(std::errc::invalid_argument);
        require(!res && res.error() == std::errc::invalid_argument,
                "an error returned as a value stays visible to the caller");
    }

    {
        auto res = co_await passThrough(std::errc::no_such_file_or_directory);
        require(!res && res.error() == std::errc::no_such_file_or_directory,
                "the error should pass through the middle layer untouched");
    }

    {
        auto res = co_await passThroughOk(41);
        require(res && *res == 42, "the success path should keep the value");
    }

    {
        // or_else 只在错误码匹配时用回调的返回值兜底，其余原样透出——fs.hpp 的
        // INVALFIX 回落就靠它只接 EINVAL。注意它的 Return 是 f() 的值（Void 的
        // operator, 是重载过的逗号），不是回调本身。
        bool ran = false;
        auto kept = (co_await failWith(std::errc::invalid_argument))
                        .or_else(std::errc::no_such_file_or_directory, [&] {
                            ran = true;
                            return 0;
                        });
        require(!kept && !ran && kept.error() == std::errc::invalid_argument,
                "or_else must not run on a different code");

        auto handled = (co_await failWith(std::errc::invalid_argument))
                           .or_else(std::errc::invalid_argument, [&] {
                               ran = true;
                               return 7;
                           });
        require(handled && *handled == 7 && ran,
                "or_else should recover on the matching code");
    }

    std::fprintf(stderr, "[test_expected] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
