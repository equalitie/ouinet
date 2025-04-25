#pragma once

#include <memory>

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/intrusive/list.hpp>

#include "../../namespaces.h"
#include "../../logger.h"
#include "../../timeout_stream.h"

namespace ouinet {
namespace ouiservice {
namespace i2poui {

class Connection : public boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>> {
public:
    Connection(const AsioExecutor& exec)
        : _exec(exec)
        , _socket(asio::ip::tcp::socket(_exec))
    {
        _socket.set_read_timeout    (std::chrono::minutes(1));
        _socket.set_write_timeout   (std::chrono::minutes(1));
        _socket.set_connect_timeout (std::chrono::minutes(1));
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    AsioExecutor get_executor() { return _exec; }

    template< class MutableBufferSequence
            , class ReadHandler>
    void async_read_some(const MutableBufferSequence&, ReadHandler&&);

    template< class ConstBufferSequence
            , class WriteHandler>
    void async_write_some(const ConstBufferSequence&, WriteHandler&&);

    void close();

    bool is_open() const { return _socket.is_open(); }

private:
    friend class Client;
    friend class Server;

    asio::ip::tcp::socket& socket() { return _socket.next_layer(); }

private:
    AsioExecutor _exec;
    TimeoutStream<asio::ip::tcp::socket> _socket;
};

template< class MutableBufferSequence
        , class ReadHandler>
inline void Connection::async_read_some( const MutableBufferSequence& bufs
                                , ReadHandler&& h)
{
  OUI_LOG_SILLY("Reading from i2p tunnel.");
    _socket.async_read_some(bufs, std::forward<ReadHandler>(h));
}

template< class ConstBufferSequence
        , class WriteHandler>
inline void Connection::async_write_some( const ConstBufferSequence& bufs
                                 , WriteHandler&& h)
{
  OUI_LOG_SILLY("Writing into i2p tunnel.");
    _socket.async_write_some(bufs, std::forward<WriteHandler>(h));
}

inline void Connection::close()
{
    sys::error_code ec;
    _socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    _socket.close(ec);
    boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>::unlink();
}

} // i2poui namespace
} // ouiservice namespace
} // ouinet namespace
