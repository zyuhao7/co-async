#pragma once
#include <co_async/std.hpp>
#include <co_async/awaiter/task.hpp>
#include <co_async/generic/cancel.hpp>
#include <co_async/generic/timeout.hpp>
#include <co_async/platform/error_handling.hpp>
#include <co_async/platform/fs.hpp>
#include <co_async/platform/pipe.hpp>
#include <co_async/platform/platform_io.hpp>
#include <co_async/utils/expected.hpp>
#include <co_async/utils/string_utils.hpp>
#include <cerrno>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace co_async {
using Pid = pid_t;

struct WaitProcessResult {
    Pid pid;
    int status;

    enum ExitType : int {
        Continued = CLD_CONTINUED,
        Stopped = CLD_STOPPED,
        Trapped = CLD_TRAPPED,
        Dumped = CLD_DUMPED,
        Killed = CLD_KILLED,
        Exited = CLD_EXITED,
        Timeout = -1,
    } exitType;
};

inline Task<Expected<>> kill_process(Pid pid, int sig = SIGKILL) {
    co_await expectError(kill(pid, sig));
    co_return {};
}

#if CO_ASYNC_INVALFIX
// IORING_OP_WAITID 要求内核 5.19。兜底改用 waitid(2)，但必须 WNOHANG 轮询：
// 直接阻塞会占死事件循环线程（连定时器都跑不到），也收不到取消信号——when_any
// 和 co_timeout 只是给取消令牌置位，不会打断正在阻塞的系统调用。
inline constexpr auto kWaitProcessSyncPoll = std::chrono::milliseconds(10);

// op 没提交时 Awaiter 停在 -ENOSYS；5.15 上提交后内核回 -EINVAL。
inline bool waitidAsyncUnavailable(int res) {
    return res == -EINVAL || res == -ENOSYS;
}

inline Task<Expected<WaitProcessResult>>
waitProcessPoll(Pid pid, int options, siginfo_t &info,
                std::optional<std::chrono::steady_clock::time_point> deadline) {
    CancelToken cancel = co_await co_cancel;
    for (;;) {
        if (cancel.is_canceled()) [[unlikely]] {
            co_return std::errc::operation_canceled;
        }
        if (waitid(P_PID, static_cast<id_t>(pid), &info, options | WNOHANG) <
            0) [[unlikely]] {
            co_return std::errc(errno);
        }
        if (info.si_pid != 0) {
            break;
        }
        if (deadline && std::chrono::steady_clock::now() >= *deadline) {
            co_return std::errc::stream_timeout;
        }
        co_await co_sleep(kWaitProcessSyncPoll);
    }
    co_return WaitProcessResult{
        .pid = info.si_pid,
        .status = info.si_status,
        .exitType = static_cast<WaitProcessResult::ExitType>(info.si_code),
    };
}
#endif

inline Task<Expected<WaitProcessResult>> wait_process(Pid pid,
                                                      int options = WEXITED) {
    siginfo_t info{};
    int res = co_await UringOp().prep_waitid(P_PID, static_cast<id_t>(pid),
                                             &info, options, 0);
#if CO_ASYNC_INVALFIX
    if (waitidAsyncUnavailable(res)) {
        co_return co_await waitProcessPoll(pid, options, info, std::nullopt);
    }
#endif
    co_await expectError(res);
    co_return WaitProcessResult{
        .pid = info.si_pid,
        .status = info.si_status,
        .exitType = static_cast<WaitProcessResult::ExitType>(info.si_code),
    };
}

inline Task<Expected<WaitProcessResult>>
wait_process(Pid pid, std::chrono::steady_clock::duration timeout,
             int options = WEXITED) {
    siginfo_t info{};
    auto ts = durationToKernelTimespec(timeout);
    auto ret = expectError(co_await UringOp::link_ops(
        UringOp().prep_waitid(P_PID, static_cast<id_t>(pid), &info, options, 0),
        UringOp().prep_link_timeout(&ts, IORING_TIMEOUT_BOOTTIME)));
#if CO_ASYNC_INVALFIX
    if (ret == std::make_error_code(std::errc::invalid_argument) ||
        ret == std::make_error_code(std::errc::function_not_supported)) {
        co_return co_await waitProcessPoll(
            pid, options, info, std::chrono::steady_clock::now() + timeout);
    }
#endif
    if (ret == std::make_error_code(std::errc::operation_canceled)) {
        co_return std::errc::stream_timeout;
    }
    co_await std::move(ret);
    co_return WaitProcessResult{
        .pid = info.si_pid,
        .status = info.si_status,
        .exitType = static_cast<WaitProcessResult::ExitType>(info.si_code),
    };
}

struct ProcessBuilder {
    ProcessBuilder() {
        mAbsolutePath = false;
        mEnvInherited = false;
        throwingErrorErrno(posix_spawnattr_init(&mAttr));
        throwingErrorErrno(posix_spawn_file_actions_init(&mFileActions));
    }

    ProcessBuilder(ProcessBuilder &&) = delete;

    ~ProcessBuilder() {
        posix_spawnattr_destroy(&mAttr);
        posix_spawn_file_actions_destroy(&mFileActions);
    }

    ProcessBuilder &chdir(std::filesystem::path path) {
        throwingErrorErrno(
            posix_spawn_file_actions_addchdir_np(&mFileActions, path.c_str()));
        return *this;
    }

    ProcessBuilder &open(int fd, FileHandle &&file) {
        open(fd, file.fileNo());
        mFileStore.push_back(std::move(file));
        return *this;
    }

    ProcessBuilder &open(int fd, FileHandle const &file) {
        return open(fd, file.fileNo());
    }

    ProcessBuilder &open(int fd, int ourFd) {
        if (fd != ourFd) {
            throwingErrorErrno(
                posix_spawn_file_actions_adddup2(&mFileActions, ourFd, fd));
        }
        return *this;
    }

    ProcessBuilder &pipe_out(int fd, OwningStream &stream) {
        int p[2];
        throwingErrorErrno(pipe2(p, 0));
        open(fd, FileHandle(p[1]));
        stream = file_from_handle(FileHandle(p[0]));
        close(p[0]);
        close(p[1]);
        return *this;
    }

    ProcessBuilder &pipe_in(int fd, OwningStream &stream) {
        int p[2];
        throwingErrorErrno(pipe2(p, 0));
        open(fd, FileHandle(p[0]));
        stream = file_from_handle(FileHandle(p[1]));
        close(p[0]);
        close(p[1]);
        return *this;
    }

    ProcessBuilder &close(int fd) {
        throwingErrorErrno(
            posix_spawn_file_actions_addclose(&mFileActions, fd));
        return *this;
    }

    ProcessBuilder &path(std::filesystem::path path, bool isAbsolute = false) {
        mPath = path.string();
        mAbsolutePath = isAbsolute;
        return *this;
    }

    ProcessBuilder &arg(std::string_view arg) {
        mArgvStore.emplace_back(arg);
        return *this;
    }

    ProcessBuilder &inherit_env(bool inherit = true) {
        if (inherit) {
            for (char *const *e = environ; *e; ++e) {
                mEnvpStore.emplace_back(*e);
            }
        }
        mEnvInherited = true;
        return *this;
    }

    ProcessBuilder &env(std::string_view key, std::string_view val) {
        if (!mEnvInherited) {
            inherit_env();
        }
        std::string env(key);
        env.push_back('=');
        env.append(val);
        mEnvpStore.emplace_back(std::move(env));
        return *this;
    }

    Task<Expected<Pid>> spawn() {
        Pid pid;
        std::vector<char *> argv;
        std::vector<char *> envp;
        if (!mArgvStore.empty()) {
            argv.reserve(mArgvStore.size() + 2);
            argv.push_back(mPath.data());
            for (auto &s: mArgvStore) {
                argv.push_back(s.data());
            }
            argv.push_back(nullptr);
        } else {
            argv = {mPath.data(), nullptr};
        }
        if (!mEnvpStore.empty()) {
            envp.reserve(mEnvpStore.size() + 1);
            for (auto &s: mEnvpStore) {
                envp.push_back(s.data());
            }
            envp.push_back(nullptr);
        }
        int status = (mAbsolutePath ? posix_spawn : posix_spawnp)(
            &pid, mPath.c_str(), &mFileActions, &mAttr, argv.data(),
            mEnvpStore.empty() ? environ : envp.data());
        if (status != 0) [[unlikely]] {
            co_return std::errc(errno);
        }
        mPath.clear();
        mArgvStore.clear();
        mEnvpStore.clear();
        mFileStore.clear();
        co_return pid;
    }

private:
    posix_spawn_file_actions_t mFileActions;
    posix_spawnattr_t mAttr;
    bool mAbsolutePath;
    bool mEnvInherited;
    std::string mPath;
    std::vector<std::string> mArgvStore;
    std::vector<std::string> mEnvpStore;
    std::vector<FileHandle> mFileStore;
};
} // namespace co_async
