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
    Connection(boost::asio::io_service& ios)
        : _ios(ios)
        , _socket(asio::ip::tcp::socket(_ios))
    {
        _socket.set_read_timeout    (std::chrono::minutes(1));
        _socket.set_write_timeout   (std::chrono::minutes(1));
        _socket.set_connect_timeout (std::chrono::minutes(1));
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    asio::io_service& get_io_service() { return _ios; }

#if BOOST_VERSION >= 106700
    asio::io_context::executor_type get_executor()   { return _socket.get_executor(); }
#endif


    template< class MutableBufferSequence
            , class ReadHandler>
    void async_read_some(const MutableBufferSequence&, ReadHandler&&);

    template< class ConstBufferSequence
            , class WriteHandler>
    void async_write_some(const ConstBufferSequence&, WriteHandler&&);

    void close();

private:
    friend class Client;
    friend class Server;

    asio::ip::tcp::socket& socket() { return _socket.next_layer(); }

private:
    asio::io_service& _ios;
    TimeoutStream<asio::ip::tcp::socket> _socket;
};

template< class MutableBufferSequence
        , class ReadHandler>
inline void Connection::async_read_some( const MutableBufferSequence& bufs
                                , ReadHandler&& h)
{
  LOG_DEBUG("Reading from i2p tunnel.");
    _socket.async_read_some(bufs, std::forward<ReadHandler>(h));
}

template< class ConstBufferSequence
        , class WriteHandler>
inline void Connection::async_write_some( const ConstBufferSequence& bufs
                                 , WriteHandler&& h)
{
  LOG_DEBUG("Writing into i2p tunnel.");
    _socket.async_write_some(bufs, std::forward<WriteHandler>(h));
}

inline void Connection::close()
{
    _socket.shutdown(asio::ip::tcp::socket::shutdown_both);
    _socket.close();
    boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>::unlink();
}

} // i2poui namespace
} // ouiservice namespace
} // ouinet namespace
