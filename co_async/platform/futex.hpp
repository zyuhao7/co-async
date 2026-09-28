#pragma once
#include <co_async/std.hpp>
#include <co_async/awaiter/task.hpp>
#include <co_async/platform/error_handling.hpp>
#include <co_async/platform/platform_io.hpp>
#include <cerrno>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace co_async {

inline constexpr std::size_t kFutexNotifyAll = static_cast<std::size_t>(std::numeric_limits<int>::max());

template <class T>
inline constexpr uint32_t getFutexFlagsFor() {
    switch (sizeof(T)) {
#ifdef FUTEX2_PRIVATE
    case sizeof(uint8_t):  return FUTEX2_SIZE_U8 | FUTEX2_PRIVATE;
    case sizeof(uint16_t): return FUTEX2_SIZE_U16 | FUTEX2_PRIVATE;
    case sizeof(uint32_t): return FUTEX2_SIZE_U32 | FUTEX2_PRIVATE;
    case sizeof(uint64_t): return FUTEX2_SIZE_U64 | FUTEX2_PRIVATE;
#else
    case sizeof(uint8_t):  return 0 | FUTEX_PRIVATE_FLAG;
    case sizeof(uint16_t): return 1 | FUTEX_PRIVATE_FLAG;
    case sizeof(uint32_t): return 2 | FUTEX_PRIVATE_FLAG;
    case sizeof(uint64_t): return 3 | FUTEX_PRIVATE_FLAG;
#endif
    }
}

template <class T>
inline constexpr uint64_t futexValueExtend(T value) {
    static_assert(std::is_trivial_v<T> && sizeof(T) <= sizeof(uint64_t));
    uint64_t ret = 0;
    std::memcpy(&ret, &value, sizeof(T));
    return ret;
}

// syscall(2) 失败时返回 -1、原因在 errno，和 io_uring 的「返回负 errno」
// 约定不同；统一转换后再交给 expectError，否则 -1 会被误读成 EPERM。
inline int syscallResult(long res) {
    return res < 0 ? -errno : static_cast<int>(res);
}

// 经典 FUTEX_WAIT_BITSET 是阻塞调用，必须给时限：否则它会永久卡住事件循环
// 线程，同一线程上的 futex_notify 再也跑不到，而那个 notify 正是唤醒它的
// 唯一手段。超时按 EAGAIN（伪唤醒）上报 —— futex API 本就允许伪唤醒，
// futex_wait 的 .ignore_error(resource_unavailable_try_again) 与调用方的
// 重检循环都能接住。
inline constexpr auto kFutexSyncWaitPoll = std::chrono::milliseconds(10);

template <class T>
inline Expected<> futex_notify_sync(std::atomic<T> *futex,
                                    std::size_t count = kFutexNotifyAll,
                                    uint32_t mask = FUTEX_BITSET_MATCH_ANY) {
#ifndef SYS_futex_wake
    const long SYS_futex_wake = 454;
#endif
    long res = syscall(SYS_futex_wake, reinterpret_cast<uint32_t *>(futex),
            static_cast<uint64_t>(count), static_cast<uint64_t>(mask),
            getFutexFlagsFor<T>());
#if CO_ASYNC_INVALFIX
    // futex_wake(454) 是 6.7 才有的系统调用，更老的内核回 ENOSYS；退到
    // 经典的 FUTEX_WAKE_BITSET（2.6.25 起可用）。第二次仍失败时不再重试，
    // errno 会原样上报。
    if (res < 0) {
        res = syscall(SYS_futex, reinterpret_cast<uint32_t *>(futex),
                FUTEX_WAKE_BITSET_PRIVATE, static_cast<uint32_t>(count), nullptr,
                nullptr, mask);
    }
#endif
    return expectError(syscallResult(res));
}

template <class T>
inline Expected<> futex_wait_sync(std::atomic<T> *futex,
                            std::type_identity_t<T> val,
                            uint32_t mask = FUTEX_BITSET_MATCH_ANY) {
#ifndef SYS_futex_wait
    const long SYS_futex_wait = 455;
#endif
    long res = syscall(SYS_futex_wait, reinterpret_cast<uint32_t *>(futex),
            futexValueExtend(val), static_cast<uint64_t>(mask),
            getFutexFlagsFor<T>());
#if CO_ASYNC_INVALFIX
    if (res < 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += kFutexSyncWaitPoll.count() * 1'000'000;
        if (ts.tv_nsec >= 1'000'000'000) {
            ts.tv_nsec -= 1'000'000'000;
            ++ts.tv_sec;
        }
        res = syscall(SYS_futex, reinterpret_cast<uint32_t *>(futex),
                FUTEX_WAIT_BITSET_PRIVATE,
                static_cast<uint32_t>(futexValueExtend(val)), &ts, nullptr,
                mask);
        if (res < 0 && (errno == ETIMEDOUT || errno == EAGAIN)) {
            return std::errc::resource_unavailable_try_again;
        }
    }
#endif
    return expectError(syscallResult(res));
}


#if CO_ASYNC_INVALFIX
// io_uring 的 FUTEX_WAIT/FUTEX_WAKE 操作码要 5.19：更老的内核回 EINVAL，
// op 压根没提交时 Awaiter 会停在 -ENOSYS（见 UringOp::Awaiter）。两种都
// 说明异步接口不可用。（-EBADF 是历史写法，一并保留。）
inline bool futexAsyncUnavailable(int res) {
    return res == -EINVAL || res == -ENOSYS || res == -EBADF;
}
#endif

template <class T>
inline Task<Expected<>> futex_wait(std::atomic<T> *futex,
                                   std::type_identity_t<T> val,
                                   uint32_t mask = FUTEX_BITSET_MATCH_ANY) {
    int res = co_await UringOp()
                  .prep_futex_wait(reinterpret_cast<uint32_t *>(futex),
                                   futexValueExtend(val),
                                   static_cast<uint64_t>(mask),
                                   getFutexFlagsFor<T>(), 0)
                  .cancelGuard(co_await co_cancel);
#if CO_ASYNC_INVALFIX
    if (futexAsyncUnavailable(res)) {
        // 同步兜底阻塞的是事件循环线程本身，异步路径那套 cancelGuard 用不上，
        // 必须自己看取消信号；否则 when_any/co_timeout 取消这次等待时它就再也
        // 回不去了。单次调用只看一眼即可：调用方（ConditionVariable::wait、
        // 各队列）都是重检循环，上一个 10ms 轮询结束后会再进来一次。
        CancelToken cancel = co_await co_cancel;
        if (cancel.is_canceled()) {
            co_return std::errc::operation_canceled;
        }
        co_return futex_wait_sync(futex, val, mask)
            .ignore_error(std::errc::resource_unavailable_try_again);
    }
#endif
    co_return expectError(res).transform([](int) {}).ignore_error(
        std::errc::resource_unavailable_try_again);
}

template <class T>
inline Task<Expected<>>
futex_notify_async(std::atomic<T> *futex,
                   std::size_t count = kFutexNotifyAll,
                   uint32_t mask = FUTEX_BITSET_MATCH_ANY) {
    int res = co_await UringOp()
                  .prep_futex_wake(reinterpret_cast<uint32_t *>(futex),
                                   static_cast<uint64_t>(count),
                                   static_cast<uint64_t>(mask),
                                   getFutexFlagsFor<T>(), 0)
                  .cancelGuard(co_await co_cancel);
#if CO_ASYNC_INVALFIX
    if (futexAsyncUnavailable(res)) {
        co_return futex_notify_sync(futex, count, mask);
    }
#endif
    co_return expectError(res).transform([](int) {});
}

template <class T>
inline void futex_notify(std::atomic<T> *futex,
                         std::size_t count = kFutexNotifyAll,
                         uint32_t mask = FUTEX_BITSET_MATCH_ANY) {
#if CO_ASYNC_INVALFIX
    futex_notify_sync(futex, count, mask);
#else
    UringOp()
        .prep_futex_wake(reinterpret_cast<uint32_t *>(futex),
                         static_cast<uint64_t>(count),
                         static_cast<uint64_t>(mask), getFutexFlagsFor<T>(), 0)
        .startDetach();
#endif
}

#if CO_ASYNC_INVALFIX
template <class>
using FutexAtomic = std::atomic<uint32_t>;
#else
template <class T>
using FutexAtomic = std::atomic<T>;
#endif

} // namespace co_async
