#pragma once
#include <co_async/std.hpp>
#include <co_async/awaiter/task.hpp>
#include <co_async/iostream/file_stream.hpp>
#include <co_async/platform/error_handling.hpp>
#include <co_async/platform/fs.hpp>
#include <co_async/platform/platform_io.hpp>
#include <co_async/platform/process.hpp>
#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace co_async {
struct FileWatch {
    enum FileEvent : std::uint32_t {
        OnAccessed = IN_ACCESS,
        OnOpened = IN_OPEN,
        OnAttributeChanged = IN_ATTRIB,
        OnModified = IN_MODIFY,
        OnDeleted = IN_DELETE_SELF,
        OnMoved = IN_MOVE_SELF,
        OnChildCreated = IN_CREATE,
        OnChildDeleted = IN_DELETE,
        OnChildMovedAway = IN_MOVED_FROM,
        OnChildMovedInto = IN_MOVED_TO,
        OnWriteFinished = IN_CLOSE_WRITE,
        OnReadFinished = IN_CLOSE_NOWRITE,
    };

    FileWatch()
        : mFile(throwingErrorErrno(inotify_init1(0))),
          mStream(file_from_handle(FileHandle(mFile))) {}

    int add(std::filesystem::path const &path, FileEvent event) {
        int wd =
            throwingErrorErrno(inotify_add_watch(mFile, path.c_str(), event));
        mWatches.emplace(wd, path);
        return wd;
    }

    FileWatch &watch(std::filesystem::path const &path, FileEvent event,
                     bool recursive = false) {
        add(path, event);
        if (recursive && std::filesystem::is_directory(path)) {
            for (auto const &entry:
                 std::filesystem::recursive_directory_iterator(path)) {
                add(entry.path(), event);
            }
        }
        return *this;
    }

    FileWatch &remove(int wd) {
        throwingErrorErrno(inotify_rm_watch(mFile, wd));
        mWatches.erase(wd);
        return *this;
    }

    struct WaitFileResult {
        std::filesystem::path path;
        FileEvent event;
    };

    Task<Expected<WaitFileResult>> wait() {
        // 双重 co_await：外层拿 Task 里的 Expected，内层由 TaskPromise 的
        // await_transform 接住——出错就地 return，不再 throw。原来读失败（尤其
        // 是被取消时的 operation_canceled）直接 throw std::runtime_error，把取消
        // 伪装成「EOF while reading struct」，没被接住就是 terminate/abort。
        co_await co_await mStream.getstruct(*mEventBuffer);
        String name;
        name.reserve(mEventBuffer->len);
        co_await co_await mStream.getn(name, mEventBuffer->len);
        name = name.c_str();
        // 不能用 at()：删掉/被移走的 watch 会回 IN_IGNORED，wd 已不在表里，
        // at() 抛 out_of_range 同样是从 Task<Expected<>> 里抛出。
        auto it = mWatches.find(mEventBuffer->wd);
        if (it == mWatches.end()) [[unlikely]] {
            co_return std::errc::no_such_file_or_directory;
        }
        auto path = it->second;
        if (!name.empty()) {
            path /= make_path(name);
        }
        co_return WaitFileResult{
            .path = std::move(path),
            .event = static_cast<FileEvent>(mEventBuffer->mask),
        };
    }

private:
    int mFile;
    OwningStream mStream;
    std::unique_ptr<struct inotify_event> mEventBuffer =
        std::make_unique<struct inotify_event>();
    std::map<int, std::filesystem::path> mWatches;
};
} // namespace co_async
