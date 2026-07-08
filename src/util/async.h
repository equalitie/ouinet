#pragma once

#include "../namespaces.h"
#include "../util/log_path.h"
#include "cancel.h"
#include "../task.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/async_result.hpp>

#include <expected>

namespace ouinet {

// This class is a successor of `asio::yield_context`. It contains
// `util::LogPath` for debugging, and `Cancel` signal so we no longer need to
// pass both to every function. But more importantly, `Async` is "cancel
// aware", meaning that we no longer need to check whether the operation has
// been cancelled explicitly.
//
// Finally, `Async` does not support the `operator[]` for returning errors.
// Instead, async operations using `Async` return `std::expected` with the
// exception that operations with handler signatures `void()` or
// `void(sys::error_code)` will return `void` and `sys::error_code`
// respectively.
class Async {
public:
    class Cancelled : public std::exception {
    public:
        Cancelled() {}

        virtual const char* what() const noexcept {
           return "Operation has been cancelled";
        }

        virtual ~Cancelled() noexcept {}
    };

    using executor_type = asio::any_io_executor;

private:
    template<class F> using DeprecatedApiResult
        = std::invoke_result_t<F, util::LogPath, Cancel, asio::yield_context>;

public:
    explicit Async(asio::yield_context asio_yield, util::LogPath log_path = {})
        : _asio_yield(asio_yield)
        , _log_path(std::move(log_path))
    {}

    explicit Async(asio::yield_context asio_yield, Cancel cancel, util::LogPath log_path = {})
        : _asio_yield(asio_yield)
        , _log_path(std::move(log_path))
        , _cancel(std::move(cancel))
    {}

    Async(Async&&) = default;
    Async& operator=(Async&&) = default;

    Async(const Async&) = default;


    util::LogPath log_path() const {
        return _log_path;
    }

    Async with_log_path(util::LogPath log_path) const {
        return Async(_asio_yield, _cancel, std::move(log_path));
    }

    Async tag(std::string t)
    {
        return with_log_path(_log_path.tag(std::move(t)));
    }

    asio::any_io_executor get_executor() const {
        return _asio_yield.get_executor();
    }

    void spawn(Cancel cancel, auto lambda) {
        task::spawn_detached(
            _asio_yield.get_executor(),
            [ lambda = std::move(lambda),
              cancel = std::move(cancel),
              // log_path = _log_path.tag("spawn")
              log_path = _log_path
            ]
            (asio::yield_context yield) mutable {
                lambda(Async(yield, std::move(cancel), std::move(log_path)));
            });
    }

    void spawn(auto lambda) {
        spawn(_cancel, std::move(lambda));
    }

    [[nodiscard]]
    Cancel::Connection cancel_slot(auto lambda) {
        return _cancel.connect(std::move(lambda));
    }

    void cancel() {
        _cancel();
    }

    const Cancel& get_cancel() const {
        return _cancel;
    }

    bool is_cancelled() const {
        return _cancel;
    }

    friend std::ostream& operator<<(std::ostream& os, const Async& y) {
        return os << y._log_path;
    }

    // For running legacy API, try not to use it unless you feel confident in
    // re-implementing the cancellation throwing logic.
    asio::yield_context asio_yield() const {
        return _asio_yield;
    }

    template<class F>
    requires(!std::same_as<DeprecatedApiResult<F>, void>)
    [[nodiscard]]
    std::expected<DeprecatedApiResult<F>, sys::error_code>
    call_deprecated(F f) {
        sys::error_code ec;
        auto ret = f(_log_path, _cancel, _asio_yield[ec]);
        if (_cancel) throw Cancelled();
        if (ec) return std::unexpected(ec);
        return ret;
    }

    template<class F>
    requires(std::same_as<DeprecatedApiResult<F>, void>)
    [[nodiscard]]
    sys::error_code
    call_deprecated(F f) {
        sys::error_code ec;
        f(_log_path, _cancel, _asio_yield[ec]);
        if (_cancel) throw Cancelled();
        return ec;
    }

private:
    template<typename, asio::completion_signature...> friend class ::boost::asio::async_result;

    asio::yield_context _asio_yield;
    util::LogPath _log_path;
    Cancel _cancel;
};

static_assert(std::is_same_v<
    asio::associated_executor_t<Async>,
    asio::any_io_executor
>);

} // ouinet namespace

// This code allows `Async` to be passed to functions expecting a
// generic completion token.
//
// Inspired by Boost.Outcome code:
// https://www.boost.org/doc/libs/1_89_0/libs/outcome/doc/html/recipes/asio-integration-1-70.html
namespace boost::asio {
    namespace detail {
        template<class E> concept IsEc = std::convertible_to<E, boost::system::error_code>;

        template<typename Sig>    struct ReturnType;
        template<>                struct ReturnType<void()>     { using type = void; };
        template<IsEc E>          struct ReturnType<void(E)>    { using type = std::expected<void, boost::system::error_code>; };
        template<IsEc E, class T> struct ReturnType<void(E, T)> { using type = std::expected<T, boost::system::error_code>; };

        // Note: in the non `void()` cases we still call the asio handler with
        // first arg being `error_code` to instruct asio that it shouldn't
        // throw on error.
        template<typename Sig>       struct ChangeSig;
        template<>                   struct ChangeSig<void()>     { using type = void(); };
        template<IsEc E>             struct ChangeSig<void(E)>    { using type = void(boost::system::error_code, std::expected<void, boost::system::error_code>); };
        template<IsEc E, typename T> struct ChangeSig<void(E, T)> { using type = void(boost::system::error_code, std::expected<T, boost::system::error_code>); };

        template<typename Handler, typename Sig> struct Wrap;

        template<typename Handler> struct Wrap<Handler, void()> {
            using executor_type = asio::associated_executor_t<Handler>;

            Handler handler;

            void operator() () {
                handler();
            }

            executor_type get_executor() const {
                return handler.get_executor();
            }
        };

        template<typename Handler, IsEc E> struct Wrap<Handler, void(E)> {
            using executor_type = asio::associated_executor_t<Handler>;

            Handler handler;

            void operator() (boost::system::error_code ec) {
                if (!ec) {
                    handler(boost::system::error_code{},
                            std::expected<void, boost::system::error_code>());
                } else {
                    handler(boost::system::error_code{}, std::unexpected(ec));
                }
            }

            executor_type get_executor() const {
                return handler.get_executor();
            }
        };

        template<typename Handler, IsEc E, typename T> struct Wrap<Handler, void(E, T)> {
            using executor_type = asio::associated_executor_t<Handler>;

            Handler handler;

            void operator() (boost::system::error_code ec, T arg) {
                if (!ec) {
                    handler(boost::system::error_code{},
                            std::expected<T, boost::system::error_code>(std::move(arg)));
                } else {
                    handler(boost::system::error_code{}, std::unexpected(ec));
                }
            }

            executor_type get_executor() const {
                return handler.get_executor();
            }
        };
    } // namespace

    template<typename Signature>
    class async_result<ouinet::Async, Signature> {
    public:
        using return_type = typename detail::ReturnType<Signature>::type;

        template<typename Initiation, typename... Args>
        requires(!std::same_as<return_type, void>)
        static return_type
        initiate(Initiation&& initiation, const ouinet::Async& token, Args&&... args)
        {
            return_type ret = async_initiate_impl(
                    std::forward<Initiation>(initiation),
                    token,
                    std::forward<Args>(args)...);

            if (token._cancel) throw ouinet::Async::Cancelled();
            return ret;
        }

        // Specialization of the above to `void` return types.
        template<typename Initiation, typename... Args>
        requires(std::same_as<return_type, void>)
        static void
        initiate(Initiation&& initiation, const ouinet::Async& token, Args&&... args)
        {
            async_initiate_impl(
                    std::forward<Initiation>(initiation),
                    token,
                    std::forward<Args>(args)...);

            if (token._cancel) throw ouinet::Async::Cancelled();
        }

    private:
        template<typename Initiation, typename... Args>
        static auto
        async_initiate_impl(Initiation&& initiation, const ouinet::Async& token, Args&&... args)
        {
            auto asio_yield = token._asio_yield;

            using OurSig = typename detail::ChangeSig<Signature>::type;

            return async_initiate<yield_context, OurSig>(
                [ init = std::forward<Initiation>(initiation)
                ]
                (auto&& handler, auto&&... call_args) mutable {
                    std::move(init)( detail::Wrap<
                                        std::decay_t<decltype(handler)>,
                                        Signature
                                     >{ std::forward<decltype(handler)>(handler) }
                                   , std::forward<decltype(call_args)>(call_args)...);
                },
                asio_yield,
                std::forward<Args>(args)...);
        }
    };
} // namespace boost::asio
