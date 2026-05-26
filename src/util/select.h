#pragma once

#include <boost/asio/async_result.hpp>
#include <optional>
#include <type_traits>
#include <variant>

#include "async.h"
#include "condition_variable.h"

namespace ouinet {

namespace detail {

template<typename T, typename... Us>
struct contains;

template<typename T>
struct contains<T> : std::false_type {};

template<typename T, typename U, typename... Us>
struct contains<T, U, Us...> : std::conditional_t<
    std::is_same_v<T, U>,
    std::true_type,
    contains<T, Us...>
> {};

template<typename T, typename Tuple>
struct prepend;

template<typename T, typename... Us>
struct prepend<T, std::tuple<Us...>> {
    using type = std::tuple<T, Us...>;
};

template<typename Tuple>
struct unique;

template<typename T>
struct unique<std::tuple<T>> {
    using type = std::tuple<T>;
};

template<typename T, typename... Us>
struct unique<std::tuple<T, Us...>> {
    using type = std::conditional_t<
        contains<T, Us...>::value,
        typename unique<std::tuple<Us...>>::type,
        typename prepend<T, typename unique<std::tuple<Us...>>::type>::type
    >;
};

template<typename Tuple>
struct tuple_to_select_result;

template<typename T>
struct tuple_to_select_result<std::tuple<T>> {
    using type = T;
};

template<typename T, typename... Ts>
struct tuple_to_select_result<std::tuple<T, Ts...>> {
    using type = std::variant<T, Ts...>;
};

template<typename... Ts>
using select_result = typename tuple_to_select_result<typename unique<std::tuple<Ts...>>::type>::type;

static_assert(std::is_same_v<select_result<bool>, bool>);
static_assert(std::is_same_v<select_result<bool, int>, std::variant<bool, int>>);

} // namespace detail

// Runs multiple coroutines concurrently waiting for the first to complete, then returns its result
// and cancels the rest.
template<typename... Fs>
auto select(Async yield, Fs... fs) {
    static_assert(sizeof...(Fs) > 0, "at least one coroutine is required");

    using Result = detail::select_result<std::invoke_result_t<Fs, Async>...>;

    ConditionVariable cv(yield.get_executor());
    std::optional<Result> result;
    Cancel cancel(yield.get_cancel());

    (
        yield.spawn(cancel, [f = std::move(fs), &cv, &result](Async yield) {
            result = Result(f(yield));
            cv.notify();
        }),
        ...
    );

    while (!result.has_value()) {
        cv.wait(yield);
    }

    // Cancel the other branches
    cancel();

    return result.value();
}

} // namespace ouinet
