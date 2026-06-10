#pragma once

#include <boost/asio/spawn.hpp>
#include <concepts>
#include <type_traits>
#include <utility>

#include "async.h"
#include "expected.h"
#include "../or_throw.h"

// Compatibility layer between the "new style" and "old style" async function.
//
// New style is a function that takes `Async` as its (typically) last argument.
// Old style is a function that takes `(..., boost::asio::yield_context)` or `(..., ouinet::YieldContext)` or
// `(..., ouinet::Cancel, boost::asio::yield_context)` or `(..., ouinet::Cancel, ouinet::YieldContext)`.
//
// Provides compatibility in both directions (new style to old style and vice versa).

namespace ouinet {
    namespace detail {

        // If `result` has value, return it. Otherwise return-or-throw the error code.
        template<typename T>
        T or_throw(
            boost::asio::yield_context& yield,
            std::expected<T, boost::system::error_code> result
        ) {
            if (result) {
                if constexpr (std::is_void_v<T>) {
                    return;
                } else {
                    return std::move(result).value();
                }
            } else {
                if constexpr (std::is_void_v<T>) {
                    return ouinet::or_throw(yield, result.error());
                } else {
                    return ouinet::or_throw(yield, result.error(), T{});
                }
            }
        }
    } // namespace detail

    template<typename F>
    struct Compat;

    template<typename F>
    Compat<F> compat(F f);

    template<typename F>
    struct Compat {
        F _f;

        // T f(error_code&) -> std::expected<T, error_code> f()
        auto operator()()
        requires std::invocable<F, boost::system::error_code&>
        {
            using R = std::invoke_result_t<F, boost::system::error_code&>;
            using E = std::expected<R, boost::system::error_code>;

            boost::system::error_code ec;

            if constexpr (std::is_void_v<R>) {
                _f(ec);

                if (ec) {
                    return E(std::unexpected(ec));
                } else {
                    return E();
                }
            } else {
                auto r = _f(ec);

                if (ec) {
                    return E(std::unexpected(ec));
                } else {
                    return E(std::move(r));
                }
            }
        }

        // T f(asio::yield_context) -> std::expected<T, error_code> f(Async)
        auto operator()(Async yield)
        requires std::invocable<F, boost::asio::yield_context>
        {
            auto result = compat(
                [
                    this,
                    yield = yield.asio_yield()
                ](boost::system::error_code& ec) {
                    return _f(yield[ec]);
                }
            )();

            if (yield.is_cancelled()) {
                throw Async::Cancelled();
            }

            return result;
        }

        // T f(Cancel, asio::yield_context) -> std::expected<T, error_code> f(Async)
        auto operator()(Async yield)
        requires std::invocable<F, Cancel, boost::asio::yield_context>
        {
            auto result = compat(
                [
                    this,
                    cancel = Cancel(yield.get_cancel()),
                    yield = yield.asio_yield()
                ](boost::system::error_code& ec) {
                    return _f(std::move(cancel), yield[ec]);
                }
            )();

            if (yield.is_cancelled()) {
                throw Async::Cancelled();
            }

            return result;
        }

        // std::expected<T, error_code> f(Async) -> T f(asio::yield_context)
        auto operator()(boost::asio::yield_context yield)
        requires std::invocable<F, Async> && is_expected_v<std::invoke_result_t<F, Async>>
        {
            auto result = _f(Async(yield));
            return detail::or_throw(yield, std::move(result));
        }

        // std::expected<T, error_code> f(Async) -> T f(Cancel, asio::yield_context)
        auto operator()(Cancel cancel, boost::asio::yield_context yield)
        requires std::invocable<F, Async> && is_expected_v<std::invoke_result_t<F, Async>>
        {
            auto result = _f(Async(yield, std::move(cancel)));
            return detail::or_throw(yield, std::move(result));
        }

        // T f(Async) -> T f(Cancel cancel, asio::yield_context)
        auto operator()(Cancel cancel, boost::asio::yield_context yield)
        requires std::invocable<F, Async>
        {
            return _f(Async(std::move(yield), std::move(cancel)));
        }
    };

    template<typename F>
    Compat<F> compat(F f) {
        return Compat { std::move(f) };
    }
} // namespace ouinet
