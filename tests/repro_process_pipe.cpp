// ProcessBuilder::pipe_in/pipe_out：把子进程当过滤器接一根管子进去、一根出来。
//
// 这里曾经怀疑是「双重关闭」——`close(p[0])` / `close(p[1])` 看着像在父进程里
// 把已经交给 stream 和 mFileStore 的 fd 再关一遍。实际不是：ProcessBuilder 自己
// 有个同名成员 `close(int)`，解析到的是它，做的是
// `posix_spawn_file_actions_addclose`——在**子进程**里、dup2 之后关掉那一端。
// 所以两端都只关在子进程该关的地方，父进程的两端分别由 pin/pout 和 mFileStore
// 持有，语义是对的。这个用例把这份语义钉住：
//
//   - 写进 pipe_in 的一行，能从 pipe_out 原样读回来（两端接对了方向）；
//   - 关掉 pipe_in 之后 cat 立刻看到 EOF 并退出 0——这要求父进程这一侧的写端
//     没有被子进程继承（`close(p[1])` 那个文件动作正是干这个的）；
//   - cat 活着的时候 /proc/<pid>/fd 里没有任何多余的管道端跟着 exec 进去
//     （0/1 是有意接的两端；只数「除 0/1/2 外的 pipe fd」是因为 ctest 自己会
//     漏一个非 O_CLOEXEC 的日志文件 fd 进来，按 0/1/2 判定会假失败）；
//   - A 的两根端还握在父进程手里时再起一个 B，B 也不能继承它们。builder 只会
//     addclose 自己的两端，A 的两端曾经靠的是 `pipe2(p, 0)`——没有 O_CLOEXEC，
//     于是 B 会拿到 A 的 read/write 端（实测落在 fd 5、6 上）。这两处现已改成
//     `pipe2(p, O_CLOEXEC)`；本用例的第二个 cat 就是钉这个。
//
// 注意第三项必须等 cat 真的 exec 完再查：spawn 返回时子进程可能还在跑文件动作，
// 那时候它的 fd 表还是从父进程拷来的原样（管道端都在），查到的是假阳性。
// 所以先读回一行——能读回来就说明 cat 已经在跑了。
//
// 跑法：cmake --build build-dbg --target repro_process_pipe -j4 && ./build-dbg/repro_process_pipe
// valgrind 下跑要带 `--undef-value-errors=no`：io_uring 的读是内核直接写进缓冲区，
// memcheck 看不见，会把刚读回来的字节报成 uninitialised（假阳性）。
#include <co_async/co_async.hpp>
#include <co_async/std.hpp>
#include <filesystem>

using namespace co_async;
using namespace std::literals;

static void require(bool ok, char const *what) {
    if (!ok) {
        std::fprintf(stderr, "[repro_process_pipe] FAIL: %s\n", what);
        std::abort();
    }
}

// 子进程的 fd 清单，只在出错时才有内容可看。
static String listFds(Pid pid) {
    String out;
    std::error_code ec;
    auto root = "/proc/" + std::to_string(pid) + "/fd";
    for (auto it = std::filesystem::directory_iterator(root, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        auto name = it->path().filename().string();
        auto target = std::filesystem::read_symlink(it->path(), ec);
        out += name + "->" + (ec ? String("?") : target.string()) + " ";
        ec.clear();
    }
    return out;
}

// 子进程里 fd>=3 且指向管道的数目——也就是「漏进来的管道端」。
//
// 不能简单地要求子进程只有 0/1/2：ctest 自己会把 `Testing/Temporary/
// LastTest.log.tmp` 以非 O_CLOEXEC 打开并让测试进程继承（排错时能在 fd 3 上
// 看到它），那是 ctest 漏的，不是本库漏的，按 0/1/2 判定会在 ctest 下假失败。
// 本库要保证的是「没有多余管道端」：0/1 是本用例有意接的两端，fd 2 是 ctest
// 捕获输出用的管道（也合法继承），其余 fd 一律不该是 pipe。
static std::size_t leakedPipeEnds(Pid pid) {
    std::size_t n = 0;
    std::error_code ec;
    auto root = "/proc/" + std::to_string(pid) + "/fd";
    for (auto it = std::filesystem::directory_iterator(root, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        auto name = it->path().filename().string();
        if (name == "0" || name == "1" || name == "2") {
            continue;
        }
        std::error_code ec2;
        auto target = std::filesystem::read_symlink(it->path(), ec2);
        if (!ec2 && target.string().starts_with("pipe:")) {
            ++n;
        }
    }
    return n;
}

// valgrind 换掉了子进程的 exec（用它自己的 trampoline），于是 posix_spawnp 在
// 「命令找不到」时不再把 ENOENT 从 spawn 的错误通道回报，而是成功返回、子进程
// 自己以 127 退出（探针实测：原生 `res=2/errno=ENOENT`；valgrind 下 `res=0`、
// 子进程 code=127）。下面那两条失败分支测的正是这个回报，valgrind 下测不到，
// 检测到就跳过——否则会把 valgrind 的进程模型差异记成库回归。
static bool runningUnderValgrind() {
    char const *preload = std::getenv("LD_PRELOAD");
    return preload && std::strstr(preload, "valgrind");
}

static Task<> amain() {
    {
        OwningStream pin, pout;
        // amain 是 Task<>（void），没有 await_transform(Expected)，所以只能
        // 单次 co_await 拿 Expected 自己查，不能写双重 co_await。
        auto pid = co_await ProcessBuilder()
                       .path("cat"sv)
                       .pipe_in(0, pin)
                       .pipe_out(1, pout)
                       .spawn();
        require(static_cast<bool>(pid), "spawn cat");
        if (!pid) {
            std::fprintf(stderr, "[repro_process_pipe] spawn -> %s\n",
                         pid.error().message().c_str());
            co_return;
        }

        require(static_cast<bool>(co_await pin.puts("hello\n"sv)), "puts");
        require(static_cast<bool>(co_await pin.flush()), "flush");

        // 读回一行即证明 cat 已经开始跑（exec 完成、管道两端也接好了），
        // 此刻查它的 fd 表才作数。
        auto line = co_await pout.getline('\n');
        require(line && *line == "hello",
                "cat should pipe the line back through the other end");

        auto leaked = leakedPipeEnds(*pid);
        if (leaked != 0) {
            std::fprintf(stderr, "[repro_process_pipe] child fds: %s\n",
                         listFds(*pid).c_str());
        }
        require(leaked == 0, "child should hold no pipe end beyond 0/1");

        // 交叉污染：A 的两根管道端还握在父进程手里时再起一个 B。B 的 builder
        // 只会 addclose 自己的两端，A 的两端得靠 O_CLOEXEC 才能在 exec 时消失；
        // 否则 B 的 fd 表里会多出 A 的 read/write 端（无 O_CLOEXEC 时实测 5、6）。
        {
            OwningStream bin, bout;
            auto b = co_await ProcessBuilder()
                         .path("cat"sv)
                         .pipe_in(0, bin)
                         .pipe_out(1, bout)
                         .spawn();
            require(static_cast<bool>(b), "spawn a second cat");
            if (b) {
                require(static_cast<bool>(co_await bin.puts("x\n"sv)), "B puts");
                require(static_cast<bool>(co_await bin.flush()), "B flush");
                auto l2 = co_await bout.getline('\n');  // 读回来即 B 已 exec
                require(l2 && *l2 == "x", "the second cat should round-trip");
                auto bleaked = leakedPipeEnds(*b);
                if (bleaked != 0) {
                    std::fprintf(stderr, "[repro_process_pipe] second child fds: %s\n",
                                 listFds(*b).c_str());
                }
                require(bleaked == 0,
                        "a second spawn must not inherit the first's pipe ends");
                co_await bin.close();
                (void) co_await wait_process(*b);
            }
        }

        co_await pin.close();   // 关掉写端，cat 才该看到 EOF 并退出
        auto waited = co_await wait_process(*pid);
        // waitid 填的是 si_status，已经是退出码本身，不用 WEXITSTATUS 解码。
        require(static_cast<bool>(waited) &&
                    waited->exitType == WaitProcessResult::Exited &&
                    waited->status == 0,
                "cat should exit cleanly after EOF");
    }

    if (!runningUnderValgrind()) {
        // 起不来的命令：posix_spawn 把错误码当返回值（不保证写 errno），这里
        // 先清 errno 让「读 errno」那条路径现形，再钉住两个失败分支都报 ENOENT。
        errno = 0;
        auto missing = co_await ProcessBuilder()
                           .path("co-async-no-such-command"sv)
                           .spawn();
        require(!missing, "spawning a missing command must fail");
        require(missing.error() == std::errc::no_such_file_or_directory,
                "a missing command should report ENOENT");

        auto badChdir = co_await ProcessBuilder()
                            .path("cat"sv)
                            .chdir("/co-async-no-such-dir"sv)
                            .spawn();
        require(!badChdir, "chdir into a missing directory must fail");
        require(badChdir.error() == std::errc::no_such_file_or_directory,
                "a failed chdir action should report ENOENT");
    }

    std::fprintf(stderr, "[repro_process_pipe] PASS\n");
    co_return;
}

int main() {
    std::setlocale(LC_ALL, "");
    co_main(amain());
    return 0;
}
