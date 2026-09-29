// 按行读一根从子进程接过来的管道——多行、每轮先清空再读，载荷必须逐行完整。
//
// 起因：examples/pipe_read.cpp 原先写的是
//
//     while (line.clear(), co_await rs.getline(line, '\n')) { ... }
//
// 跑起来 277 行 `process output: ` 后面全是空的。根因不在库：GCC 的协程 lowering
// 在**循环条件**里遇到 co_await 时，会把条件里 co_await 那个操作数的求值排到条件
// 里其余子表达式**之前**——逗号左臂 `line.clear()` 于是落在 getline 已经把整行
// append 进 line 之后执行，刚读到的行被清掉。GCC 上游是在案的：PR c++/101027
// （"Short-circuit behavior not respected with co_await in while head"），2021-06
// 起未修复，2024-10 又补了一个逗号操作数的复现；只在 while/for 头里出现，if 头、
// 赋值、普通函数调用都不受影响（本机 GCC 13.3.0 实测）。
//
// 所以本用例钉的是「安全写法」：清空放进循环体、作为独立语句排在 co_await 之前。
// 谁要是把它改回逗号条件，本用例就会看到空载荷而失败。用例本身同时覆盖
// getline 的多行切分与每轮清空后的语义（不累积、不丢行）。
//
// 跑法：cmake --build build-dbg --target repro_pipe_getline_lines -j4
//       && ./build-dbg/repro_pipe_getline_lines
// valgrind 下要带 `--undef-value-errors=no`（io_uring 内核直写缓冲区，memcheck
// 看不见，会把读回来的字节报成 uninitialised）。
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>

using namespace co_async;
using namespace std::literals;

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[repro_pipe_getline_lines] FAIL: %s\n", what);
        std::abort();
    }
}

static Task<> amain() {
    OwningStream pin, pout;
    // amain 是 Task<>（void），没有 await_transform(Expected)，只能单次 co_await
    // 拿 Expected 自己查。
    auto pid = co_await ProcessBuilder()
                   .path("cat"sv)
                   .pipe_in(0, pin)
                   .pipe_out(1, pout)
                   .spawn();
    require(static_cast<bool>(pid), "spawn cat");
    if (!pid) {
        std::fprintf(stderr, "[repro_pipe_getline_lines] spawn -> %s\n",
                     pid.error().message().c_str());
        co_return;
    }

    // 三个非空行 + 一个空行 + 一个长行：既查载荷整体没丢，也查空行是「长度 0」
    // 而不是「被误当成失败/被吞掉」，还查一行读到最后一个字节都没被截断。
    String const payload =
        "alpha\nbeta\ngamma\n\nthe quick brown fox jumps over the lazy dog\n";
    require(static_cast<bool>(co_await pin.puts(payload)), "puts payload");
    require(static_cast<bool>(co_await pin.flush()), "flush payload");
    co_await pin.close();  // 写端关掉，cat 才会在看到 EOF 后收尾

    String const expected[] = {
        "alpha", "beta", "gamma", "",
        "the quick brown fox jumps over the lazy dog",
    };

    String line;
    std::size_t i = 0;
    for (;;) {
        // 清空放在这里：独立语句，排在 co_await 之前，是 GCC 那个循环条件 bug
        // 的绕法。不要改成 `while (line.clear(), co_await ...)`。
        line.clear();
        auto e = co_await pout.getline('\n');
        if (!e) {
            break;
        }
        require(i < std::size(expected), "getline returned more lines than written");
        if (*e != expected[i]) {
            std::fprintf(stderr,
                         "[repro_pipe_getline_lines] line %zu: got [%s] (%zu bytes), "
                         "want [%s]\n",
                         i, e->c_str(), e->size(), expected[i].c_str());
        }
        require(*e == expected[i], "line payload should round-trip intact");
        ++i;
    }
    require(i == std::size(expected), "getline should return every line written");
    // 最后一轮 clear() 之后再读到 EOF 时，line 里不该残留上一行的尾巴。
    require(line.empty(), "line should be empty after the final clear + EOF");

    (void) co_await wait_process(*pid);
    std::fprintf(stderr, "[repro_pipe_getline_lines] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
