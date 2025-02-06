#pragma once

#include <list>
#include <iostream>
#include <boost/asio/buffer.hpp>
#include <boost/utility/string_view.hpp>
#include "../logger.h"
#include "../namespaces.h"
#include "../or_throw.h"
#include "../util/condition_variable.h"
#include "../async_sleep.h"
#include "rate_counter.h"
#include "../util/handler_tracker.h"

namespace ouinet { namespace bittorrent {

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
    UdpMultiplexer(asio_utp::udp_multiplexer&&);

    AsioExecutor get_executor();

    void send(std::string&& message, const udp::endpoint& to, Cancel&, asio::yield_context);
    void send(std::string&& message, const udp::endpoint& to);

    // NOTE: The pointer inside the returned string_view is guaranteed to
    // be valid only until the next coroutine based async IO call or until
    // the coroutine that runs this function exits (whichever comes first).
    const boost::string_view receive(udp::endpoint& from, Cancel&, asio::yield_context);

    udp::endpoint local_endpoint() const { return _socket.local_endpoint(); }

    ~UdpMultiplexer();

    bool is_v4() const { return _socket.local_endpoint().address().is_v4(); }
    bool is_v6() const { return _socket.local_endpoint().address().is_v6(); }

private:
    void maintain_max_rate_bytes_per_sec( float current_rate
                                        , float max_rate
                                        , asio::yield_context);

    static
    boost::asio::const_buffer buffer(const std::string& s) {
        return boost::asio::buffer(const_cast<const char*>(s.data()), s.size());
    }

private:
    asio_utp::udp_multiplexer _socket;
    std::list<SendEntry> _send_queue;
    ConditionVariable _send_queue_nonempty;
    IntrusiveList<RecvEntry> _receive_queue;
    Signal<void()> _terminate_signal;
    asio::steady_timer _rate_limiting_timer;
    RateCounter _rc_rx;
    RateCounter _rc_tx;
    float sent = 0;
    float recv = 0;
};

inline
UdpMultiplexer::UdpMultiplexer(asio_utp::udp_multiplexer&& s):
    _socket(std::move(s)),
    _send_queue_nonempty(_socket.get_executor()),
    _rate_limiting_timer(_socket.get_executor())
{
    assert(_socket.is_open());

    LOG_INFO("BT is operating on endpoint: UDP:", _socket.local_endpoint());

#if 0
    asio::spawn(get_executor(), [this] (asio::yield_context yield) {
            using namespace std::chrono;
            using std::cerr;

            Cancel cancel(_terminate_signal);

            auto print_rate = [](float r) {
                if      (r >= 1000000) cerr << (r / 1000000) << "MiB/s";
                else if (r >= 1000)    cerr << (r / 1000)    << "KiB/s";
                else                   cerr << (r)           << "B/s";
            };

            while (true) {
                sys::error_code ec;
                async_sleep(get_executor(), seconds(1), cancel, yield[ec]);
                if (cancel) return;

                cerr << "Current BT rate rx:";
                print_rate(_rc_rx.rate());
                cerr << " (" << recv << ") tx:";
                print_rate(_rc_tx.rate());
                cerr << " (" << sent << ") send_queue size:" << _send_queue.size();
                cerr << "\n";
                sent = 0;
                recv = 0;
            }
    }, asio::detached);
#endif

    TRACK_SPAWN(get_executor(), [this] (asio::yield_context yield) {
        Cancel cancel(_terminate_signal);

        auto terminated = cancel.connect([&] {
            _send_queue_nonempty.notify();
        });

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

            if (!ec) {
                _socket.async_send_to(buffer(entry.message), entry.to, yield[ec]);
            }

            if (terminated) break;

            if (!ec) {
                sent += entry.message.size();
                _rc_tx.update(entry.message.size());
                maintain_max_rate_bytes_per_sec(_rc_tx.rate(), max_rate, yield[ec]);
                if (terminated) break;
            }

            _send_queue.front().sent_signal(ec);
            _send_queue.pop_front();
        }
    });

    TRACK_SPAWN(get_executor(), [this] (asio::yield_context yield) {
        auto terminated = _terminate_signal.connect([]{});

        std::vector<uint8_t> buf;
        udp::endpoint from;

        buf.resize(65536);

        while (true) {
            sys::error_code ec;


            size_t size = _socket.async_receive_from(asio::buffer(buf), from, yield[ec]);
            if (terminated) return;

            _rc_rx.update(size);
            recv += size;

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

    _rate_limiting_timer.expires_after(delay);
    _rate_limiting_timer.async_wait(yield);
}

inline
UdpMultiplexer::~UdpMultiplexer()
{
    _terminate_signal();

    sys::error_code ec;
    _socket.close(ec);
}

inline
void UdpMultiplexer::send(
    std::string&& message,
    const udp::endpoint& to,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    ConditionVariable condition(get_executor());

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
UdpMultiplexer::receive(udp::endpoint& from, Cancel& cancel, asio::yield_context yield)
{
    ConditionVariable condition(get_executor());

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

    auto cancelled = cancel.connect([&] {
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
AsioExecutor UdpMultiplexer::get_executor()
{
    return _socket.get_executor();
}

}} // namespaces
