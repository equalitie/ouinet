#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/utility/string_view.hpp>
#include "../namespaces.h"
#include "../or_throw.h"
#include "../util/condition_variable.h"
#include "rate_counter.h"

namespace ouinet { namespace bittorrent {

static
boost::asio::const_buffers_1 buffer(const std::string& s) {
    return boost::asio::buffer(const_cast<const char*>(s.data()), s.size());
}

class UdpMultiplexer {
private:
    using udp = asio::ip::udp;

    using IntrusiveHook = boost::intrusive::list_base_hook
        <boost::intrusive::link_mode
            <boost::intrusive::auto_unlink>>;

    template<class T>
    using IntrusiveList = boost::intrusive::list
        <T, boost::intrusive::constant_time_size<false>>;

    struct SendEntry {
        std::string message;
        udp::endpoint to;
        Signal<void(sys::error_code)> sent_signal;
    };

    struct RecvEntry : IntrusiveHook {
        std::function<void(
            sys::error_code,
            boost::string_view,
            udp::endpoint
        )> handler;
    };

public:
    UdpMultiplexer(udp::socket&&);

    asio::io_service& get_io_service();

    void send(std::string&& message, const udp::endpoint& to, asio::yield_context yield, Signal<void()>& cancel_signal);
    void send(std::string&& message, const udp::endpoint& to, asio::yield_context yield)
        { Signal<void()> cancel_signal; send(std::move(message), to, yield, cancel_signal); }
    void send(std::string&& message, const udp::endpoint& to);

    // NOTE: The pointer inside the returned string_view is guaranteed to
    // be valid only until the next coroutine based async IO call or until
    // the coroutine that runs this function exits (whichever comes first).
    const boost::string_view receive(udp::endpoint& from, asio::yield_context yield, Signal<void()>& cancel_signal);
    const boost::string_view receive(udp::endpoint& from, asio::yield_context yield)
        { Signal<void()> cancel_signal; return receive(from, yield, cancel_signal); }

    ~UdpMultiplexer();

private:
    void maintain_max_rate_bytes_per_sec( float current_rate
                                        , float max_rate
                                        , asio::yield_context);

private:
    udp::socket _socket;
    std::list<SendEntry> _send_queue;
    ConditionVariable _send_queue_nonempty;
    IntrusiveList<RecvEntry> _receive_queue;
    Signal<void()> _terminate_signal;
    asio::steady_timer _rate_limiting_timer;
};

inline
UdpMultiplexer::UdpMultiplexer(udp::socket&& s):
    _socket(std::move(s)),
    _send_queue_nonempty(_socket.get_io_service()),
    _rate_limiting_timer(_socket.get_io_service())
{
    assert(_socket.is_open());

    asio::spawn(get_io_service(), [this] (asio::yield_context yield) {
        auto terminated = _terminate_signal.connect([&] {
            _send_queue_nonempty.notify();
        });

        RateCounter rc;

        const float max_rate = (500 * 1000)/8; // 500K bits/sec

        while(true) {
            if (terminated) {
                break;
            }

            if (_send_queue.empty()) {
                sys::error_code ec;
                _send_queue_nonempty.wait(yield[ec]);
                continue;
            }

            SendEntry& entry = _send_queue.front();

            sys::error_code ec;
            _socket.async_send_to(buffer(entry.message), entry.to, yield[ec]);

            if (terminated) break;

            if (!ec) {
                rc.update(entry.message.size());
                maintain_max_rate_bytes_per_sec(rc.rate(), max_rate, yield[ec]);
                if (terminated) break;
            }

            _send_queue.front().sent_signal(ec);
            _send_queue.pop_front();
        }
    });

    asio::spawn(get_io_service(), [this] (asio::yield_context yield) {
        auto terminated = _terminate_signal.connect([]{});

        std::vector<uint8_t> buf;
        udp::endpoint from;

        while (true) {
            sys::error_code ec;

            buf.resize(65536);

            size_t size = _socket.async_receive_from(asio::buffer(buf), from, yield[ec]);
            if (terminated) {
                break;
            }

            buf.resize(size);
            for (auto& entry : std::move(_receive_queue)) {
                entry.handler(ec, boost::string_view((char*)&buf[0], size), from);
            }
        }
    });
}

inline
void UdpMultiplexer::maintain_max_rate_bytes_per_sec( float current_rate
                                                    , float max_rate
                                                    , asio::yield_context yield)
{
    if (current_rate <= max_rate) return;

    float delay_sec = current_rate/max_rate - 1;
    auto delay = std::chrono::milliseconds(int(delay_sec*1000));

    _rate_limiting_timer.expires_from_now(delay);
    _rate_limiting_timer.async_wait(yield);
}

inline
UdpMultiplexer::~UdpMultiplexer()
{
    _terminate_signal();
    _socket.close();
}

inline
void UdpMultiplexer::send(
    std::string&& message,
    const udp::endpoint& to,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    ConditionVariable condition(get_io_service());

    sys::error_code ec;

    _send_queue.emplace_back();
    _send_queue.back().message = std::move(message);
    _send_queue.back().to = to;
    auto sent_slot = _send_queue.back().sent_signal.connect([&] (sys::error_code ec_) {
        ec = ec_;
        condition.notify();
    });

    auto cancelled = cancel_signal.connect([&] {
        condition.notify();
    });

    auto terminated = _terminate_signal.connect([&] {
        condition.notify();
    });

    _send_queue_nonempty.notify();
    condition.wait(yield);

    if (cancelled || terminated) {
        or_throw(yield, asio::error::operation_aborted);
    }

    if (ec) {
        or_throw(yield, ec);
    }
}

inline
void UdpMultiplexer::send(
    std::string&& message,
    const udp::endpoint& to
) {
    _send_queue.emplace_back();
    _send_queue.back().message = std::move(message);
    _send_queue.back().to = to;

    _send_queue_nonempty.notify();
}

inline
const boost::string_view
UdpMultiplexer::receive(udp::endpoint& from, asio::yield_context yield, Signal<void()>& cancel_signal)
{
    ConditionVariable condition(get_io_service());

    sys::error_code ec;
    boost::string_view buffer;

    RecvEntry recv_entry;
    recv_entry.handler = [&](sys::error_code ec_, boost::string_view buffer_, udp::endpoint from_) {
        ec = ec_;
        buffer = buffer_;
        from = from_;
        condition.notify();
    };
    _receive_queue.push_back(recv_entry);

    auto cancelled = cancel_signal.connect([&] {
        condition.notify();
    });

    auto terminated = _terminate_signal.connect([&] {
        condition.notify();
    });

    condition.wait(yield);

    if (cancelled || terminated) {
        return or_throw<boost::string_view>(yield, asio::error::operation_aborted);
    }

    if (ec) {
        return or_throw<boost::string_view>(yield, ec);
    }

    return buffer;
}

inline
asio::io_service& UdpMultiplexer::get_io_service()
{
    return _socket.get_io_service();
}

}} // namespaces
