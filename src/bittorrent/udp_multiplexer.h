#pragma once

#include <boost/utility/string_view.hpp>
#include "../namespaces.h"
#include "../or_throw.h"
#include "../util/condition_variable.h"

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

    struct SendEntry : IntrusiveHook {
        virtual void operator()(asio::yield_context) = 0;
    };

    using RecvHandlerSig = void( sys::error_code
                               , std::pair< boost::string_view
                                          , udp::endpoint>);

    struct RecvEntry : IntrusiveHook {
        std::function<RecvHandlerSig> handler;
    };

    struct SendLoop : std::enable_shared_from_this<SendLoop> {
        SendLoop(asio::io_service& ios) : queue_cv(ios) {}
        void start();
        bool stopped = false;
        ConditionVariable queue_cv;
        IntrusiveList<SendEntry> queue;
    };

    struct RecvLoop : std::enable_shared_from_this<RecvLoop> {
        void start(std::shared_ptr<udp::socket>);
        IntrusiveList<RecvEntry> queue;
    };

public:
    UdpMultiplexer(udp::socket);

    UdpMultiplexer(const UdpMultiplexer&) = delete;

    asio::io_service& get_io_service();

    template<class Buffers>
    void send(const Buffers&, const udp::endpoint&, asio::yield_context);

    // NOTE: The the pointer inside the returned string_view is guaranteed to
    // be valid only until the next coroutine based async IO call or until
    // the coroutine that runs this function exits (whichever comes first).
    const boost::string_view receive(udp::endpoint&, asio::yield_context);

    ~UdpMultiplexer();

private:
    // XXX: Having three shared_ptrs is overkill.
    std::shared_ptr<udp::socket> _socket;
    std::shared_ptr<SendLoop> _send_loop;
    std::shared_ptr<RecvLoop> _recv_loop;
};

inline
UdpMultiplexer::UdpMultiplexer(udp::socket s)
    : _socket(std::make_shared<udp::socket>(std::move(s)))
    , _send_loop(std::make_shared<SendLoop>(_socket->get_io_service()))
    , _recv_loop(std::make_shared<RecvLoop>())
{
    assert(_socket->is_open());

    _send_loop->start();
    _recv_loop->start(_socket);
}

inline
UdpMultiplexer::~UdpMultiplexer()
{
    _socket->close();
    _send_loop->stopped = true;
    if (_send_loop->queue.empty()) _send_loop->queue_cv.notify();
}

inline
void UdpMultiplexer::SendLoop::start() {
    asio::spawn( queue_cv.get_io_service()
               , [this, self = shared_from_this()] (asio::yield_context yield) {
        while (true) {
            if (queue.empty()) {
                if (stopped) break;
                sys::error_code ec;
                queue_cv.wait(yield[ec]);
            }

            if (stopped) break;

            auto& entry = queue.front();
            queue.pop_front();
            entry(yield);
        }
    });
}

template<class Buffers>
inline
void UdpMultiplexer::send( const Buffers& buf
                         , const udp::endpoint& to
                         , asio::yield_context yield)
{
    ConditionVariable write_cv(_socket->get_io_service());

    struct SendEntry_ : SendEntry {
        udp::socket& socket;
        ConditionVariable& write_cv;
        const Buffers& buf;
        const udp::endpoint& to;

        SendEntry_( udp::socket& socket
                  , ConditionVariable& write_cv
                  , const Buffers& buf
                  , const udp::endpoint& to)
            : socket(socket), write_cv(write_cv), buf(buf), to(to) {}

        void operator()(asio::yield_context yield) override {
            sys::error_code ec;
            socket.async_send_to(buf, to, yield[ec]);
            write_cv.notify(ec);
        }
    };

    if (_send_loop->queue.empty()) {
        _send_loop->queue_cv.notify();
    }

    SendEntry_ entry(*_socket, write_cv, buf, to); 
    _send_loop->queue.push_back(entry);

    sys::error_code ec;
    write_cv.wait(yield[ec]);

    return or_throw(yield, ec);
}

inline
void UdpMultiplexer::RecvLoop::start(std::shared_ptr<udp::socket> socket)
{
    auto& ios = socket->get_io_service();

    asio::spawn( ios
               , [ this
                 , socket = std::move(socket)
                 , self = shared_from_this()
                 ] (asio::yield_context yield) {
        constexpr size_t max_buf_size = 65536;

        std::vector<uint8_t> buf;

        udp::endpoint from;

        while (true) {
            sys::error_code ec;

            buf.resize(max_buf_size);

            size_t size = socket->async_receive_from( asio::buffer(buf)
                                                    , from
                                                    , yield[ec]);

            buf.resize(size);

            // The handlers might add new entries into the queue and we don't
            // want to execute those yet.
            auto q = std::move(queue);

            while (!q.empty()) {
                auto& entry = q.front();
                auto h = std::move(entry.handler);
                q.pop_front();
                h(ec, std::make_pair( boost::string_view((char*)&buf[0], size)
                                    , from));
            }

            if (ec) break;
        }
    });
}

inline
const boost::string_view
UdpMultiplexer::receive(udp::endpoint& from, asio::yield_context yield)
{
    boost::asio::async_completion
        < asio::yield_context
        , RecvHandlerSig
        > init(yield);

    RecvEntry recv_entry;

    recv_entry.handler = std::move(init.completion_handler);

    _recv_loop->queue.push_back(recv_entry);

    auto pair = init.result.get();
    from = pair.second;
    return pair.first;
}

inline
asio::io_service& UdpMultiplexer::get_io_service()
{
    return _socket->get_io_service();
}

}} // namespaces
