#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

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
        template<typename F>
        requires std::invocable<F, boost::system::error_code&>
        auto invoke_expected(F f) {
            using R = std::invoke_result_t<F, boost::system::error_code&>;
            using E = std::expected<R, boost::system::error_code>;

            boost::system::error_code ec;

            if constexpr (std::is_void_v<R>) {
                f(ec);

                if (ec) {
                    return E(std::unexpected(ec));
                } else {
                    return E();
                }
            } else {
                auto r = f(ec);

                if (ec) {
                    return E(std::unexpected(ec));
                } else {
                    return E(std::move(r));
                }
            }
        }
    }

    template<typename F>
    struct Compat {
        F _f;

        auto operator()(Async yield)
        requires std::invocable<F, boost::asio::yield_context>
        {
            return detail::invoke_expected(
                [
                    this,
                    yield = yield.asio_yield()
                ](boost::system::error_code& ec) {
                    return _f(yield[ec]);
                }
            );
        }

        auto operator()(Async yield)
        requires std::invocable<F, Cancel, boost::asio::yield_context>
        {
            return detail::invoke_expected(
                [
                    this,
                    cancel = Cancel(yield.get_cancel()),
                    yield = yield.asio_yield()
                ](boost::system::error_code& ec) {
                    return _f(std::move(cancel), yield[ec]);
                }
            );
        }

        auto operator()(Cancel cancel, boost::asio::yield_context yield)
        requires std::invocable<F, Async> && is_expected_v<std::invoke_result_t<F, Async>>
        {
            using R = std::invoke_result_t<F, Async>;
            using V = typename R::value_type;

            auto result = _f(Async(std::move(yield), std::move(cancel)));

            if (result) {
                if constexpr (std::is_void_v<V>) {
                    return;
                } else {
                    return std::move(result).value();
                }
            } else {
                if constexpr (std::is_void_v<V>) {
                    return or_throw(yield, result.error());
                } else {
                    return or_throw(yield, result.error(), V{});
                }
            }
        }

        auto operator()(Cancel cancel, boost::asio::yield_context yield)
        requires std::invocable<F, Async> && std::is_void_v<std::invoke_result_t<F, Async>>
        {
            _f(Async(std::move(yield), std::move(cancel)));
        }
    };

    template<typename F>
    Compat<F> compat(F f) {
        return Compat { std::move(f) };
    }

} // namespace ouinet
