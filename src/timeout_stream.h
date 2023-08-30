#pragma once

#include <boost/asio/steady_timer.hpp>
#include <boost/optional.hpp>

namespace ouinet {

/*
 * A wrapper around any stream (such as the asio::tcp::socket)
 * that adds a timeout ability to reading and writing.
 *
 * Usage:
 *
 *     tcp::socket s = my_make_connected_socket(yield);
 *     TimeoutStream t(std::move(s));
 *     t.set_read_timeout(std::chrono::seconds(10));
 *
 *     // If nothing arrives within 10 seconds, this function
 *     // will finish with the asio::error::timed_out error.
 *     t.async_read_some(my_buffer, yield[ec]);
 */

using ouinet::util::AsioExecutor;

template<class InnerStream> class TimeoutStream {
public:
    using executor_type = AsioExecutor;
    using next_layer_type = InnerStream;
    using endpoint_type = typename InnerStream::endpoint_type;

private:
    using Timer     = boost::asio::steady_timer;
    using Clock     = typename Timer::clock_type;
    using Duration  = typename Timer::duration;
    using TimePoint = typename Timer::time_point;
    using Handler   = std::function<void(const sys::error_code&, size_t)>;
    using ConnectHandler = std::function<void(const sys::error_code&)>;

    class Deadline : public std::enable_shared_from_this<Deadline> {
        using Parent = std::enable_shared_from_this<Deadline>;
    public:
        Deadline(AsioExecutor& exec)
            : _timer(exec)
        {}

        void start(Duration d, std::function<void()> h)
        {
            _handler = std::move(h);

            _desired_deadline = Clock::now() + d;

            if (_expires_at) {
                if (*_desired_deadline < _expires_at) {
                    _timer.cancel();
                }

                return;
            }

            _expires_at = _desired_deadline;
            _timer.expires_at(*_expires_at);

            _timer.async_wait(
                [this, self = Parent::shared_from_this()]
                (const sys::error_code&) { on_timer(); });
        }

        void stop() {
            _handler = nullptr;
            _desired_deadline = boost::none;

            if (_expires_at) {
                _expires_at = Clock::now();
                _timer.cancel();
            }
        }

    private:
        void on_timer() {
            _expires_at = boost::none;

            if (!_desired_deadline) {
                return;
            }

            auto now = Clock::now();

            if (now < *_desired_deadline) {
                return start(*_desired_deadline - now, std::move(_handler));
            }

            auto h = std::move(_handler);
            h();
        }

    private:
        Timer _timer;
        boost::optional<TimePoint> _expires_at;
        boost::optional<TimePoint> _desired_deadline;
        std::function<void()> _handler;
    };

    struct State {
        InnerStream inner;

        // XXX: Use shared_ptr's aliasing constructor to avoid
        // unnecessary allocations.
        std::shared_ptr<Deadline> read_deadline;
        std::shared_ptr<Deadline> write_deadline;
        std::shared_ptr<Deadline> connect_deadline;

        Handler read_handler;
        Handler write_handler;
        ConnectHandler connect_handler;

        State(InnerStream&& in)
            : inner(std::move(in))
        {
            auto exec = inner.get_executor();

            read_deadline    = std::make_shared<Deadline>(exec);
            write_deadline   = std::make_shared<Deadline>(exec);
            connect_deadline = std::make_shared<Deadline>(exec);
        }

        bool is_open() const { return inner.is_open(); }
    };

public:
    TimeoutStream() {}
    TimeoutStream(InnerStream&&);

    TimeoutStream(const TimeoutStream&) = delete;
    TimeoutStream& operator=(const TimeoutStream&) = delete;

    TimeoutStream(TimeoutStream&&) = default;
    TimeoutStream& operator=(TimeoutStream&&) = default;

    executor_type get_executor()
    {
        return _state->inner.get_executor();
    }

    template<class MutableBufferSequence, class Token>
    auto async_read_some(const MutableBufferSequence&, Token&&);

    template<class ConstBufferSequence, class Token>
    auto async_write_some(const ConstBufferSequence&, Token&&);

    template<class Token>
    auto async_connect(const endpoint_type&, Token&&);

    // Set the timeout for all consecutive read operations.
    void set_read_timeout(Duration d) {
        _max_read_duration = d;
    }

    // Set the timeout for all consecutive read operations.
    template<class DurationT>
    void set_read_timeout(boost::optional<DurationT> d) {
        if (d) _max_read_duration = Duration(*d);
        else   _max_read_duration = boost::none;
    }

    // Set the timeout for all consecutive write operations.
    void set_write_timeout(Duration d) {
        _max_write_duration = d;
    }

    // Set the timeout for all consecutive write operations.
    template<class DurationT>
    void set_write_timeout(boost::optional<DurationT> d) {
        if (d) _max_write_duration = Duration(*d);
        else   _max_write_duration = boost::none;
    }

    // Set the timeout for all consecutive write operations.
    void set_connect_timeout(Duration d) {
        _max_connect_duration = d;
    }

    // Set the timeout for all consecutive write operations.
    template<class DurationT>
    void set_connect_timeout(boost::optional<DurationT> d) {
        if (d) _max_connect_duration = Duration(*d);
        else   _max_connect_duration = boost::none;
    }

    void close(sys::error_code& ec)
    {
        if (!_state) return;

        if (_state->inner.is_open()) {
            _state->inner.close(ec);
        }
    }

          next_layer_type& next_layer()       { return _state->inner; }
    const next_layer_type& next_layer() const { return _state->inner; }

    template<typename ShutdownType>
    void shutdown(ShutdownType type, sys::error_code& ec) {
        _state->inner.shutdown(type, ec);
    }

    bool is_open() const {
        if (_state) return false;
        return _state->is_open();
    }

    ~TimeoutStream();

private:

    template<class Handler>
    void on_read(Handler&&, const boost::system::error_code&, size_t);

    template<class Handler>
    void on_write(Handler&&, const boost::system::error_code&, size_t);

    void setup_deadline( boost::optional<Duration>
                       , Deadline&
                       , std::function<void()>);

private:
    std::shared_ptr<State> _state;
    boost::optional<Duration> _max_read_duration;
    boost::optional<Duration> _max_write_duration;
    boost::optional<Duration> _max_connect_duration;
};

template<class InnerStream>
inline
TimeoutStream<InnerStream>::TimeoutStream(InnerStream&& inner_stream)
    : _state(std::make_shared<State>(std::move(inner_stream)))
{
}

template<class InnerStream>
template<class MutableBufferSequence, class Token>
inline
auto TimeoutStream<InnerStream>::async_read_some
    ( const MutableBufferSequence& bs
    , Token&& token)
{
    using Sig = void(const sys::error_code&, size_t);

    boost::asio::async_completion<Token, Sig> init(token);

    _state->read_handler = std::move(init.completion_handler);

    setup_deadline(_max_read_duration, *_state->read_deadline, [s = _state] {
        auto h = std::move(s->read_handler);
        s->inner.close();
        h(asio::error::timed_out, 0);
    });

    _state->inner.async_read_some( bs
                                 , [s = _state]
                                   (const sys::error_code& ec, size_t size) {
                                       s->read_deadline->stop();
                                       if (s->read_handler) {
                                           auto h = std::move(s->read_handler);
                                           h(ec, size);
                                       }
                                   });

    return init.result.get();
}

template<class InnerStream>
template<class ConstBufferSequence, class Token>
inline
auto TimeoutStream<InnerStream>::async_write_some( const ConstBufferSequence& bs
                                                 , Token&& token)
{
    using Sig = void(const sys::error_code&, size_t);

    boost::asio::async_completion<Token, Sig> init(token);

    _state->write_handler = std::move(init.completion_handler);

    setup_deadline(_max_write_duration, *_state->write_deadline, [s = _state] {
        auto h = std::move(s->write_handler);
        s->inner.close();
        h(asio::error::timed_out, 0);
    });

    _state->inner.async_write_some( bs
                                  , [s = _state]
                                    (const sys::error_code& ec, size_t size) {
                                        s->write_deadline->stop();
                                        if (s->write_handler) {
                                            auto h = std::move(s->write_handler);
                                            h(ec, size);
                                        }
                                    });

    return init.result.get();
}

template<class InnerStream>
template<class Token>
inline
auto TimeoutStream<InnerStream>::async_connect
    (const endpoint_type& ep, Token&& token)
{
    using Sig = void(const sys::error_code&);

    boost::asio::async_completion<Token, Sig> init(token);

    _state->connect_handler = std::move(init.completion_handler);

    setup_deadline(_max_connect_duration, *_state->connect_deadline, [s = _state] {
        auto h = std::move(s->connect_handler);
        s->inner.close();
        h(asio::error::timed_out);
    });

    _state->inner.async_connect( ep
                               , [s = _state]
                                 (const sys::error_code& ec) {
                                     s->connect_deadline->stop();
                                     if (s->connect_handler) {
                                         auto h = std::move(s->connect_handler);
                                         h(ec);
                                     }
                                 });

    return init.result.get();
}

template<class InnerStream>
void TimeoutStream<InnerStream>::setup_deadline( boost::optional<Duration> d
                                               , Deadline& deadline
                                               , std::function<void()> handler)
{
    if (!d) return;
    deadline.start(*d, std::move(handler));
}

template<class InnerStream>
inline TimeoutStream<InnerStream>::~TimeoutStream()
{
    sys::error_code ec;
    close(ec);
}

} // namespace
