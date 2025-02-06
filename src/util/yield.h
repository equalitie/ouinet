#pragma once

#include <sstream>
#include "../namespaces.h"
#include "../util/executor.h"
#include "../util/str.h"
#include "../logger.h"
#include "../or_throw.h"
#include <boost/intrusive/list.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/utility/string_view.hpp>
#include <boost/optional.hpp>

namespace ouinet {

using ouinet::util::AsioExecutor;

class Yield : public boost::intrusive::list_base_hook
              < boost::intrusive::link_mode<boost::intrusive::auto_unlink>>
{
    using Clock = std::chrono::steady_clock;

    using List = boost::intrusive::list
        <Yield, boost::intrusive::constant_time_size<false>>;

    struct TimeoutState {
        Yield* self;
        asio::steady_timer timer;

        TimeoutState(const AsioExecutor& ex, Yield* self)
            : self(self)
            , timer(ex)
        {}

        void stop() {
            self = nullptr;
            timer.cancel();
        }
    };

public:
    Yield( asio::io_context& ctx
         , asio::yield_context asio_yield
         , std::string con_id = "")
        : Yield(ctx.get_executor(), asio_yield, std::move(con_id))
    {}

    Yield(const AsioExecutor& ex
         , asio::yield_context asio_yield
         , std::string con_id = "")
        : _ex(ex)
        , _asio_yield(asio_yield)
        , _ignored_error(std::make_shared<sys::error_code>())
        , _tag(util::str("R", generate_context_id()))
        , _parent(nullptr)
        , _start_time(Clock::now())
    {
        if (!con_id.empty()) {
            _tag = con_id + "/" + _tag;
        }

        start_timing();
    }

    Yield(Yield& parent)
        : Yield(parent, parent._asio_yield)
    {
    }

    Yield detach(asio::yield_context yield) {
        return Yield(*this, yield);
    }

private:
    Yield(Yield& parent, asio::yield_context asio_yield)
        : _ex(parent._ex)
        , _asio_yield(asio_yield)
        , _ignored_error(parent._ignored_error)
        , _tag(parent.tag())
        , _parent(&parent)
        , _start_time(Clock::now())
    {
        parent._children.push_back(*this);
    }

public:
    Yield(Yield&& y)
        : _ex(y._ex)
        , _asio_yield(y._asio_yield)
        , _ignored_error(std::move(y._ignored_error))
        , _tag(std::move(y._tag))
        , _parent(&y)
        , _timeout_state(std::move(y._timeout_state))
        , _start_time(y._start_time)
    {
        if (_timeout_state) {
            _timeout_state->self = this;
        }

        y._children.push_back(*this);
    }

    Yield tag(std::string t)
    {
        Yield ret(*this);
        ret._tag = tag() + "/" + t;
        ret.start_timing();
        return ret;
    }

    const std::string& tag() const
    {
        if (_tag.empty()) {
            assert(_parent);
            return _parent->tag();
        }
        return _tag;
    }

    Yield operator[](sys::error_code& ec)
    {
        return {*this, _asio_yield[ec]};
    }

    explicit operator asio::yield_context() const
    {
        return _asio_yield;
    }

    Yield ignore_error()
    {
        return {*this, _asio_yield[*_ignored_error]};
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

    ~Yield()
    {
        if (_children.empty()) {
            stop_timing();
        }

        auto chs = std::move(_children);

        for (auto& ch : chs) {
            assert(ch._parent == this);
            ch._parent = _parent;
        }

        if (_parent) {
            while (!chs.empty()) {
                auto& ch = chs.front();
                chs.pop_front();
                _parent->_children.push_back(ch);
            }

            // At least this node has to be on parent.
            assert(_parent->_children.size() >= 1);

            if (_parent->_children.size() == 1) {
                _parent->start_timing();
            }
        }
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

    static size_t generate_context_id()
    {
        static size_t next_id = 0;
        return next_id++;
    }

    void start_timing();
    void stop_timing();

    static uint64_t duration_secs(Clock::duration d) {
        return std::chrono::duration_cast<std::chrono::seconds>(d).count();
    }

private:
    AsioExecutor _ex;
    asio::yield_context _asio_yield;
    std::shared_ptr<sys::error_code> _ignored_error;
    std::string _tag;
    Yield* _parent;
    std::shared_ptr<TimeoutState> _timeout_state;
    List _children;
    Clock::time_point _start_time;
};

inline
void Yield::stop_timing()
{
    if (!_timeout_state) {
        if (_parent) _parent->stop_timing();
        return;
    }

    _timeout_state->stop();
    _timeout_state = nullptr;
}

inline
void Yield::start_timing()
{
    Clock::duration timeout = std::chrono::seconds(30);

    stop_timing();

    _timeout_state = std::make_shared<TimeoutState>(_ex, this);

    asio::spawn(_ex
               , [ ts = _timeout_state, timeout]
                 (asio::yield_context yield) {

            if (!ts->self) return;

            auto notify = [&](Clock::duration d) {
                LOG_WARN(ts->self->tag()
                        , " is still working after "
                        , Yield::duration_secs(d), " seconds");
            };

            boost::optional<Clock::duration> first_duration
                = Clock::now() - ts->self->_start_time;

            if (*first_duration >= timeout) {
                notify(*first_duration);
            }

            while (ts->self) {
                sys::error_code ec; // ignored

                ts->timer.expires_after(timeout);

                ts->timer.async_wait(yield[ec]);

                if (!ts->self) break;

                notify(Clock::now() - ts->self->_start_time);
            }
        });
}

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

        logger.log(log_level, util::str(tag(), " ", str.substr(0, endl)));

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
