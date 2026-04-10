#pragma once

#include "../namespaces.h"
#include "../util/executor.h"
#include "../util/str.h"
#include "../util/log_path.h"
#include "../logger.h"
#include "../or_throw.h"
#include "../task.h"
#include <boost/asio/spawn.hpp>
#include <boost/utility/string_view.hpp>

namespace ouinet {

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

    asio::any_io_executor get_executor() const {
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

template<class... Args>
inline
void YieldContext::log(log_level_t log_level, Args&&... args)
{
    if (logger.get_threshold() > log_level)
        return;  // avoid string conversion early

    YieldContext::log(log_level, boost::string_view(util::str(std::forward<Args>(args)...)));
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
