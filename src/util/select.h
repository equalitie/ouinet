#pragma once

#include <boost/asio/steady_timer.hpp>
#include <optional>
#include <type_traits>
#include <variant>

#include "async.h"
#include "async_sleep.h"
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
static_assert(std::is_same_v<select_result<bool, bool>, bool>);
static_assert(std::is_same_v<select_result<bool, int>, std::variant<bool, int>>);
static_assert(std::is_same_v<select_result<bool, bool, int>, std::variant<bool, int>>);

template<typename T, typename E>
std::expected<T, E> maybe_flatten(std::expected<T, E> e) {
    return std::move(e);
}

template<typename T, typename E0, typename E1>
requires std::convertible_to<E1, E0>
std::expected<T, E0> maybe_flatten(std::expected<std::expected<T, E0>, E1> e) {
    if (e) {
        return std::move(e.value());
    } else {
        return std::expected<T, E0>(std::unexpected(std::move(e.error())));
    }
}

// Holder for the return value of `select`. Can't use `std::optional` directly because it doesn't
// work with `void`.
template<typename T>
struct SelectResultHolder {
    std::optional<T> result;

    template<typename F>
    void assign(F f, Async yield) {
        auto r = T(f(yield));

        if (!result) {
            result = std::move(r);
        }
    }

    operator bool() const {
        return result.has_value();
    }

    T&& get() && {
        return std::move(result).value();
    }
};

template<>
struct SelectResultHolder<void> {
    bool result = false;

    template<typename F>
    void assign(F f, Async yield) {
        f(yield);
        result = true;
    }

    operator bool() const {
        return result;
    }

    void get() && {}
};


} // namespace detail

// Runs multiple coroutines concurrently waiting for the first to complete, then returns its result
// and cancels the rest.
template<typename... Fs>
auto select(Async yield, Fs... fs) {
    static_assert(sizeof...(Fs) > 0, "at least one coroutine is required");

    using Result = detail::select_result<std::invoke_result_t<Fs, Async>...>;

    // RAII guard that notifies about branch completion.
    struct BranchGuard {
        size_t& remaining;
        ConditionVariable& cv;

        ~BranchGuard() {
            --remaining;
            cv.notify();
        }
    };

    detail::SelectResultHolder<Result> holder;
    size_t remaining = sizeof...(Fs);
    ConditionVariable cv(yield.get_executor());

    // Wait for all branches to complete. This needs to run at scope exit even in case of exception
    // to ensure all of the coroutines spawned from this function complete before this function
    // exits.
    //
    // Note we can't use RAII for this because we might need to throw `Async::Cancelled`
    // afterwards and it's UB to throw exceptions from destructors.
    auto cleanup = [&remaining, &cv] (Async yield) {
        while (remaining > 0) {
            std::ignore = cv.wait(yield);
        }
    };

    Async branch_yield(yield);

    (
        branch_yield.spawn([f = std::move(fs), &holder, &remaining, &cv] (Async yield) {
            BranchGuard guard { remaining, cv };
            holder.assign(std::move(f), yield);
        }),
        ...
    );

    try {
        // Wait for the first branch to complete.
        while (!holder) {
            cv.wait(yield).value();
        }

        // Cancel the other branches and wait for them to complete.
        branch_yield.cancel();
        cleanup(yield.suppress_cancel());

        // If this whole `select` has been cancelled during the above `cleanup`, we need to
        // propagate the `Cancelled` exception.
        if (yield.is_cancelled()) {
            throw Async::Cancelled();
        }

        return std::move(holder).get();
    } catch (...) {
        // Ensure all coroutines are completed before rethrowing the exception.

        if (!branch_yield.is_cancelled()) {
            branch_yield.cancel();
        }

        cleanup(yield.suppress_cancel());
        throw;
    }
}

// Error returned from `timeout` when the timeout expires before the coroutine was able to complete.
struct Expired {
    operator boost::system::error_code () const {
        return boost::asio::error::timed_out;
    }
};

static_assert(std::is_convertible_v<Expired, boost::system::error_code>);

inline std::ostream& operator<<(std::ostream& os, const Expired&) {
    return os << "timeout expired";
}

// Runs a coroutine to completion or timeout, whichever happens first.
template<typename F>
auto timeout(boost::asio::steady_timer::duration duration, F f, Async yield) {
    using R = std::invoke_result_t<F, Async>;
    using E = std::expected<R, Expired>;

    auto result = select(
        yield,
        [duration](auto yield) {
            async_sleep(duration, yield);
            return E(std::unexpected(Expired {}));
        },
        [f = std::move(f)](auto yield) {
            return E(f(yield));
        }
    );

    return detail::maybe_flatten(std::move(result));
}


} // namespace ouinet
