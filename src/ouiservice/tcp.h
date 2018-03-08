#pragma once

#include <list>
#include <boost/asio/ip/tcp.hpp>

#include "../ouiservice.h"

namespace ouinet {
namespace ouiservice {

class TcpOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    TcpOuiServiceServer(boost::asio::io_service& ios, boost::asio::ip::tcp::endpoint endpoint);

    void start_listen(asio::yield_context yield);
    void stop_listen();

    GenericConnection accept(asio::yield_context yield);

    private:
    boost::asio::io_service& _ios;
    boost::asio::ip::tcp::acceptor _acceptor;
    boost::asio::ip::tcp::endpoint _endpoint;
};


class TcpOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    TcpOuiServiceClient(boost::asio::io_service& ios, boost::asio::ip::tcp::endpoint endpoint);

    GenericConnection connect(asio::yield_context yield);
    void cancel_connect();

    private:
    boost::asio::io_service& _ios;
    boost::asio::ip::tcp::endpoint _endpoint;
    std::list<boost::asio::ip::tcp::socket*> _connecting_sockets;
};

} // ouiservice namespace
} // ouinet namespace
