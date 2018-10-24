#pragma once

#include <sstream>
#include "../util.h"
#include <boost/intrusive/list.hpp>

namespace ouinet {

class Yield : public boost::intrusive::list_base_hook
              < boost::intrusive::link_mode<boost::intrusive::auto_unlink>>
{
    using Clock = std::chrono::steady_clock;

    using List = boost::intrusive::list
        <Yield, boost::intrusive::constant_time_size<false>>;

    struct TimeoutState {
        Yield* self;
        asio::steady_timer timer;

        TimeoutState(asio::io_service& ios, Yield* self)
            : self(self)
            , timer(ios)
        {}

        void stop() {
            self = nullptr;
            timer.cancel();
        }
    };

public:
    Yield(asio::io_service& ios
         , asio::yield_context asio_yield
         , std::string con_id = "")
        : _ios(ios)
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

private:
    Yield(Yield& parent, asio::yield_context asio_yield)
        : _ios(parent._ios)
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
        : _ios(y._ios)
        , _asio_yield(y._asio_yield)
        , _ignored_error(std::move(y._ignored_error))
        , _tag(std::move(y._tag))
        , _parent(y._parent)
        , _timeout_state(std::move(y._timeout_state))
        , _children(std::move(y._children))
        , _start_time(y._start_time)
    {
        if (_timeout_state) {
            _timeout_state->self = this;
        }

        for (auto& ch : _children) {
            ch._parent = this;
        }

        y._parent = nullptr;
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

    operator asio::yield_context() const
    {
        return _asio_yield;
    }

    Yield ignore_error()
    {
        return {*this, _asio_yield[*_ignored_error]};
    }

    ~Yield()
    {
        // Chldren must be destroyed before this.
        assert(_children.size() == 0);
        stop_timing();

        if (_parent) {
            // At least this node has to be on parent.
            assert(_parent->_children.size() >= 1);

            if (_parent->_children.size() == 1) {
                _parent->start_timing();
            }
        }
    }

    template<class... Args>
    void log(Args&&...);
    void log(beast::string_view);

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
    asio::io_service& _ios;
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

    _timeout_state = std::make_shared<TimeoutState>(_ios, this);

    asio::spawn(_ios
               , [ ts = _timeout_state, timeout]
                 (asio::yield_context yield) {

            if (!ts->self) return;

            auto notify = [&](Clock::duration d) {
                std::cerr << ts->self->tag()
                          << " is still working after "
                          << Yield::duration_secs(d) << " seconds"
                          << std::endl;
            };

            boost::optional<Clock::duration> first_duration
                = Clock::now() - ts->self->_start_time;

            if (*first_duration >= timeout) {
                notify(*first_duration);
            }

            while (ts->self) {
                sys::error_code ec; // ignored

                ts->timer.expires_from_now(timeout);

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
    Yield::log(beast::string_view(util::str(std::forward<Args>(args)...)));
}

inline
void Yield::log(beast::string_view str)
{
    using beast::string_view;

    while (str.size()) {
        auto endl = str.find('\n');

        std::cerr << tag() << " " << str.substr(0, endl) << std::endl;

        if (endl == std::string::npos) {
            break;
        }

        str = str.substr(endl+1);
    }
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
