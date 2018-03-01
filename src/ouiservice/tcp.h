#pragma once

#include <boost/asio/ip/tcp.hpp>

#include "../ouiservice.h"

namespace ouinet {
namespace ouiservice {

class TcpOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    TcpOuiServiceServer(boost::asio::io_service& ios, boost::asio::ip::tcp::endpoint endpoint);

    void start_listen(asio::yield_context yield);
    void stop_listen(asio::yield_context yield);

    GenericConnection accept(asio::yield_context yield);
    void cancel_accept();

    bool is_accepting();

    private:
    boost::asio::io_service& _ios;
    boost::asio::ip::tcp::acceptor _acceptor;
    boost::asio::ip::tcp::endpoint _endpoint;
    bool _in_accept;
};

/*
class TcpOuiServiceClient
{
    public:
    GenericConnection connect(asio::yield_context yield) = 0;
    void cancel_connect(asio::yield_context yield) = 0;
};
*/

} // ouiservice namespace
} // ouinet namespace
