#pragma once
#include <co_async/std.hpp>
#include <co_async/awaiter/concepts.hpp>
#include <co_async/awaiter/task.hpp>
#include <co_async/awaiter/when_all.hpp>
#include <co_async/generic/cancel.hpp>
#include <co_async/generic/generic_io.hpp>
#include <co_async/generic/timeout.hpp>

namespace co_async {

template <class T>
struct WhenAnyResult {
    T value;
    std::size_t index;
};

template <Awaitable T, class Alloc = std::allocator<T>>
Task<WhenAnyResult<typename AwaitableTraits<T>::AvoidRetType>>
when_any(std::vector<T, Alloc> const &tasks) {
    using Avoid = typename AwaitableTraits<T>::AvoidRetType;
    using TaskAlloc =
        typename std::allocator_traits<Alloc>::template rebind_alloc<Task<>>;
    CancelSource cancel(co_await co_cancel);
    // 不能用 vector<Task<>, Alloc>(size, alloc)：Alloc 的 value_type 是调用方的
    // 任务类型（例如 Task<int>），和元素类型 Task<> 不同，实例化就撞 vector 的
    // 静态断言；而且 (count, alloc) 构造会先塞进 tasks.size() 个空 Task（空
    // coroutine_handle），when_all 一 co_await 它们就是野句柄。
    TaskAlloc alloc(tasks.get_allocator());
    std::vector<Task<>, TaskAlloc> newTasks(alloc);
    newTasks.reserve(tasks.size());
    std::optional<Avoid> result;
    std::size_t index = static_cast<std::size_t>(-1);
    std::size_t i = 0;
    // 捕获 &tasks + i（而不是循环变量 task 的引用），并把 result.emplace 放到
    // 取消判定之后：否则先跑完的那个会调 cancel()，被取消的那个醒来后仍会
    // 用自己（被取消得来的）返回值把胜者的结果覆盖掉，胜者的 index 配败者的
    // value。变参版 when_any 就是这个顺序。
    for (; i < tasks.size(); ++i) {
        newTasks.push_back(co_cancel.bind(
            cancel,
            co_bind([&tasks, &result, &index, i,
                     cancel = cancel.token()]() mutable -> Task<> {
                auto res = (co_await std::move(tasks[i]), Void());
                if (cancel.is_canceled()) {
                    co_return;
                }
                co_await cancel.cancel();
                index = i;
                result.emplace(std::move(res));
            })));
    }
    co_await when_all(newTasks);
    co_return {std::move(result.value()), index};
}

template <Awaitable... Ts>
Task<std::variant<typename AwaitableTraits<Ts>::AvoidRetType...>>
when_any(Ts &&...tasks) {
    return co_bind(
        [&]<std::size_t... Is>(std::index_sequence<Is...>)
            -> Task<
                std::variant<typename AwaitableTraits<Ts>::AvoidRetType...>> {
            CancelSource cancel(co_await co_cancel);
            std::optional<
                std::variant<typename AwaitableTraits<Ts>::AvoidRetType...>>
                result;
            co_await when_all(co_cancel.bind(
                cancel,
                co_bind([&result, task = std::move(tasks)]() mutable -> Task<> {
                    auto res = (co_await std::move(task), Void());
                    if (co_await co_cancel) {
                        co_return;
                    }
                    co_await co_cancel.cancel();
                    result.emplace(std::in_place_index<Is>, std::move(res));
                }))...);
            co_return std::move(result.value());
        },
        std::make_index_sequence<sizeof...(Ts)>());
}

template <Awaitable... Ts, class Common = std::common_type_t<
                               typename AwaitableTraits<Ts>::AvoidRetType...>>
Task<WhenAnyResult<Common>> when_any_common(Ts &&...tasks) {
    return co_bind(
        [&]<std::size_t... Is>(
            std::index_sequence<Is...>) -> Task<WhenAnyResult<Common>> {
            CancelSource cancel(co_await co_cancel);
            std::size_t index = static_cast<std::size_t>(-1);
            std::optional<Common> result;
            co_await when_all(co_cancel.bind(
                cancel, co_bind([&index, &result,
                                 task = std::move(tasks)]() mutable -> Task<> {
                    auto res = (co_await std::move(task), Void());
                    if (co_await co_cancel) {
                        co_return;
                    }
                    co_await co_cancel.cancel();
                    index = Is;
                    result.emplace(std::move(res));
                }))...);
            co_return WhenAnyResult<Common>{std::move(result.value()), index};
        },
        std::make_index_sequence<sizeof...(Ts)>());
}

template <Awaitable A, class Timeout>
Task<std::conditional_t<
    std::convertible_to<std::errc, typename AwaitableTraits<A>::RetType> &&
        !std::is_void_v<typename AwaitableTraits<A>::RetType>,
    typename AwaitableTraits<A>::RetType,
    Expected<typename AwaitableTraits<A>::RetType>>>
co_timeout(A &&a, Timeout timeout) {
    auto res = co_await when_any(std::forward<A>(a), co_sleep(timeout));
    if (auto ret = std::get_if<0>(&res)) {
        if constexpr (std::is_void_v<typename AwaitableTraits<A>::RetType>) {
            co_return {};
        } else {
            co_return std::move(*ret);
        }
    } else {
        co_return std::errc::stream_timeout;
    }
}

} // namespace co_async
