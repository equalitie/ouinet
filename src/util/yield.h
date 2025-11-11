#pragma once

#ifdef _WIN32
#pragma push_macro("Yield")
#undef Yield
#endif

#include <sstream>
#include "../namespaces.h"
#include "../util/executor.h"
#include "../util/str.h"
#include "../util/log_tree.h"
#include "../logger.h"
#include "../or_throw.h"
#include "../task.h"
#include <boost/asio/spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/utility/string_view.hpp>
#include <boost/optional.hpp>

namespace ouinet {

using ouinet::util::AsioExecutor;

class Yield : public boost::intrusive::list_base_hook
              < boost::intrusive::link_mode<boost::intrusive::auto_unlink>>
{
public:
    Yield( asio::yield_context asio_yield, util::LogTree log_tree = {})
        : _asio_yield(asio_yield)
        , _ignored_error(std::make_shared<sys::error_code>())
        , _log_tree(std::move(log_tree))
    {}

    Yield(const Yield&) = default;

public:
    Yield tag(std::string t)
    {
        Yield ret(_asio_yield, _log_tree.tag(std::move(t)));
        return ret;
    }

    util::LogTree log_tree() const {
        return _log_tree;
    }

    Yield operator[](sys::error_code& ec)
    {
        return Yield(_asio_yield[ec], _log_tree);
    }

    explicit operator asio::yield_context() const
    {
        return _asio_yield;
    }

    Yield ignore_error()
    {
        return Yield(_asio_yield[*_ignored_error], _log_tree);
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
    template<class F>
    auto
    run(F&& f) {
        return std::forward<F>(f)(_asio_yield);
    }

    // Log for the given level, when enabled.
    template<class... Args>
    void log(log_level_t, Args&&...);
    void log(log_level_t, boost::string_view);

    // These log at INFO level, when enabled, for backwards compatibility.
    template<class... Args>
    void log(Args&&...);
    void log(boost::string_view);

private:
    asio::yield_context _asio_yield;
    std::shared_ptr<sys::error_code> _ignored_error;
    util::LogTree _log_tree;
};

template<class... Args>
inline
void Yield::log(Args&&... args)
{
    if (logger.get_threshold() > INFO)
        return;  // avoid string conversion early

    Yield::log(INFO, boost::string_view(util::str(std::forward<Args>(args)...)));
}

inline
void Yield::log(boost::string_view str)
{
    Yield::log(INFO, str);
}

template<class... Args>
inline
void Yield::log(log_level_t log_level, Args&&... args)
{
    if (logger.get_threshold() > log_level)
        return;  // avoid string conversion early

    Yield::log(log_level, boost::string_view(util::str(std::forward<Args>(args)...)));
}

inline
void Yield::log(log_level_t log_level, boost::string_view str)
{
    using boost::string_view;

    if (logger.get_threshold() > log_level)
        return;

    while (str.size()) {
        auto endl = str.find('\n');

        logger.log(log_level, util::str(_log_tree, " ", str.substr(0, endl)));

        if (endl == std::string::npos) {
            break;
        }

        str = str.substr(endl+1);
    }
}


template<class Ret>
inline
Ret or_throw( Yield yield
            , const sys::error_code& ec
            , Ret&& ret = {})
{
    return or_throw(static_cast<asio::yield_context>(yield), ec, std::forward<Ret>(ret));
}

inline
void or_throw( Yield yield
             , const sys::error_code& ec)
{
    return or_throw(static_cast<asio::yield_context>(yield), ec);
}

} // ouinet namespace


namespace boost { namespace asio {

template<class Sig>
class async_result<::ouinet::Yield, Sig>
    : public async_result<asio::yield_context, Sig>
{
    using Super = async_result<asio::yield_context, Sig>;

public:
    explicit async_result(typename Super::completion_handler_type& h)
        : Super(h) {}
};

}} // boost::asio namespace

#ifdef _WIN32
#pragma pop_macro("Yield")
#endif
