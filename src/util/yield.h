#pragma once

#include <sstream>
#include "../namespaces.h"
#include "../util/executor.h"
#include "../util/str.h"
#include "../util/log_path.h"
#include "../logger.h"
#include "../or_throw.h"
#include "../task.h"
#include <boost/asio/spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/utility/string_view.hpp>
#include <boost/optional.hpp>

namespace ouinet {

using ouinet::util::AsioExecutor;

class YieldContext
{
public:
    explicit YieldContext( asio::yield_context asio_yield, util::LogPath log_path = {})
        : _asio_yield(asio_yield)
        , _ignored_error(std::make_shared<sys::error_code>())
        , _log_path(std::move(log_path))
    {}

    YieldContext(const YieldContext&) = default;

    YieldContext tag(std::string t)
    {
        return YieldContext(_asio_yield, _log_path.tag(std::move(t)));
    }

    util::LogPath log_path() const {
        return _log_path;
    }

    YieldContext operator[](sys::error_code& ec)
    {
        return YieldContext(_asio_yield[ec], _log_path);
    }

    AsioExecutor get_executor() const {
        return _asio_yield.get_executor();
    }

    asio::yield_context native() const {
        return _asio_yield;
    }

    YieldContext throwing() const {
        YieldContext ret = *this;
        ret._asio_yield.ec_ = nullptr;
        return ret;
    }

    YieldContext ignore_error()
    {
        return YieldContext(_asio_yield[*_ignored_error], _log_path);
    }

    // Use this to keep this instance (with tag, tracking, etc.) alive
    // while running code which only accepts plain `asio::yield_context`.
    //
    // Example:
    //
    //     auto foo = yield[ec].tag("foo").run([&] (auto y) { return do_foo(a, y); });
    //
    // Where `do_foo` only accepts `asio::yield_context`.
    //
    // You can spare some boilerplate by defining a macro like:
    //
    //     #define YIELD_KEEP(_Y, _C) ((_Y).run([&] (auto __Y) { return (_C); }));
    //
    // And using it like:
    //
    //     auto foo = YIELD_KEEP(yield[ec].tag("foo"), do_foo(a, __Y));
    //
    // TODO: This function is deprecated, use `native()` instead.
    template<class F>
    auto
    run(F&& f) {
        return std::forward<F>(f)(_asio_yield);
    }

    template<class F> void spawn_detached(F&& lambda) {
        auto log_path = _log_path.tag("spawn");
        task::spawn_detached(
            _asio_yield.get_executor(),
            [ f = std::move(lambda)
            , log_path = std::move(log_path)
            ]
            (asio::yield_context yield) mutable {
                f(YieldContext(yield, log_path));
            });
    }

    // Log for the given level, when enabled.
    template<class... Args>
    void log(log_level_t, Args&&...);
    void log(log_level_t, boost::string_view);

    // These log at INFO level, when enabled, for backwards compatibility.
    template<class... Args>
    void log(Args&&...);
    void log(boost::string_view);

    friend std::ostream& operator<<(std::ostream& os, const YieldContext& y) {
        return os << y._log_path;
    }

private:
    asio::yield_context _asio_yield;
    std::shared_ptr<sys::error_code> _ignored_error;
    util::LogPath _log_path;
};

template<class... Args>
inline
void YieldContext::log(Args&&... args)
{
    if (logger.get_threshold() > INFO)
        return;  // avoid string conversion early

    YieldContext::log(INFO, boost::string_view(util::str(std::forward<Args>(args)...)));
}

inline
void YieldContext::log(boost::string_view str)
{
    YieldContext::log(INFO, str);
}

template<class... Args>
inline
void YieldContext::log(log_level_t log_level, Args&&... args)
{
    if (logger.get_threshold() > log_level)
        return;  // avoid string conversion early

    YieldContext::log(log_level, boost::string_view(util::str(std::forward<Args>(args)...)));
}

inline
void YieldContext::log(log_level_t log_level, boost::string_view str)
{
    using boost::string_view;

    if (logger.get_threshold() > log_level)
        return;

    while (str.size()) {
        auto endl = str.find('\n');

        logger.log(log_level, util::str(_log_path, " ", str.substr(0, endl)));

        if (endl == std::string::npos) {
            break;
        }

        str = str.substr(endl+1);
    }
}


template<class Ret>
inline
Ret or_throw( YieldContext yield
            , const sys::error_code& ec
            , Ret&& ret = {})
{
    return or_throw(yield.native(), ec, std::forward<Ret>(ret));
}

inline
void or_throw( YieldContext yield
             , const sys::error_code& ec)
{
    return or_throw(yield.native(), ec);
}

} // ouinet namespace
