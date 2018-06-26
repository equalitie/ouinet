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
template<class InnerStream> class TimeoutStream {
public:
    using executor_type = asio::io_context::executor_type;

private:
    using Timer     = boost::asio::steady_timer;
    using Clock     = typename Timer::clock_type;
    using Duration  = typename Timer::duration;
    using TimePoint = typename Timer::time_point;

    class Deadline : public std::enable_shared_from_this<Deadline> {
        using Parent = std::enable_shared_from_this<Deadline>;
    public:
        Deadline(asio::io_context& ioc)
            : _timer(ioc)
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

        std::shared_ptr<Deadline> read_deadline;
        std::shared_ptr<Deadline> write_deadline;

        State(InnerStream&& in)
            : inner(std::move(in))
        {
            auto& ctx = inner.get_executor().context();

            read_deadline  = std::make_shared<Deadline>(ctx);
            write_deadline = std::make_shared<Deadline>(ctx);
        }
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

    void close()
    {
        if (!_state) return;

        if (_state->inner.is_open()) {
            _state->inner.close();
        }
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
    using namespace std;

    namespace asio = boost::asio;
    namespace sys  = boost::system;

    using Sig     = void(sys::error_code, size_t);
    using Result  = asio::async_result<Token, Sig>;
    using Handler = typename Result::completion_handler_type;

    struct ReadState {
        Handler handler;
        bool timed_out;
    };

    // XXX: Handler is non-copyable, but can we do this without allocation?
    auto recv_state = make_shared<ReadState>(
            ReadState{forward<Token>(token), false});

    Result result(recv_state->handler);

    setup_deadline(_max_read_duration, *_state->read_deadline, [recv_state] {
        recv_state->timed_out = true;
        recv_state->handler(asio::error::timed_out, 0);
    });

    _state->inner.async_read_some( bs
                                 , [s = _state, recv_state]
                                   (const sys::error_code& ec, size_t size) {
                                       s->read_deadline->stop();
                                       if (!recv_state->timed_out)
                                           recv_state->handler(ec, size);
                                   });

    return result.get();
}

template<class InnerStream>
template<class ConstBufferSequence, class Token>
inline
auto TimeoutStream<InnerStream>::async_write_some( const ConstBufferSequence& bs
                                                 , Token&& token)
{
    using namespace std;

    namespace asio = boost::asio;
    namespace sys  = boost::system;

    using Sig     = void(sys::error_code, size_t);
    using Result  = asio::async_result<Token, Sig>;
    using Handler = typename Result::completion_handler_type;

    struct WriteState {
        Handler handler;
        bool timed_out;
    };

    // XXX: Handler is non-copyable, but can we do this without allocation?
    auto write_state = make_shared<WriteState>(
            WriteState{forward<Token>(token), false});

    Result result(write_state->handler);

    setup_deadline(_max_write_duration, *_state->write_deadline, [write_state] {
        write_state->timed_out = true;
        write_state->handler(asio::error::timed_out, 0);
    });

    _state->inner.async_write_some( bs
                                  , [s = _state, write_state]
                                    (const sys::error_code& ec, size_t size) {
                                        s->write_deadline->stop();
                                        if (!write_state->timed_out)
                                            write_state->handler(ec, size);
                                    });

    return result.get();
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
    close();
}

} // namespace
