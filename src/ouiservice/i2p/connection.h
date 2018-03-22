#pragma once

#include <memory>

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/intrusive/list.hpp>

#include "../../namespaces.h"

namespace ouinet {
namespace ouiservice {
namespace i2poui {

class Connection : public boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>> {
public:
    Connection(boost::asio::io_service& ios)
        : _ios(ios)
        , _socket(_ios)
    {}

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    asio::io_service& get_io_service() { return _ios; }

    asio::ip::tcp::socket& socket() { return _socket; }

    template< class MutableBufferSequence
            , class ReadHandler>
    void async_read_some(const MutableBufferSequence&, ReadHandler&&);

    template< class ConstBufferSequence
            , class WriteHandler>
    void async_write_some(const ConstBufferSequence&, WriteHandler&&);

    void close();

private:
    asio::io_service& _ios;
    asio::ip::tcp::socket _socket;
};

template< class MutableBufferSequence
        , class ReadHandler>
inline void Connection::async_read_some( const MutableBufferSequence& bufs
                                , ReadHandler&& h)
{
    _socket.async_read_some(bufs, std::forward<ReadHandler>(h));
}

template< class ConstBufferSequence
        , class WriteHandler>
inline void Connection::async_write_some( const ConstBufferSequence& bufs
                                 , WriteHandler&& h)
{
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
