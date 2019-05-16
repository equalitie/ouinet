#pragma once

#include <list>
#include <boost/asio/ip/udp.hpp>
#include <boost/optional.hpp>
#include <asio_utp.hpp>

#include "../ouiservice.h"
#include "../util/async_queue.h"

namespace ouinet {
namespace ouiservice {

class UtpOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    UtpOuiServiceServer(asio::io_service& ios, asio::ip::udp::endpoint endpoint);

    void start_listen(asio::yield_context) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context) override;

    ~UtpOuiServiceServer();

    private:
    asio::io_service& _ios;
    asio::ip::udp::endpoint _endpoint;
    Cancel _cancel;
    util::AsyncQueue<asio_utp::socket> _accept_queue;
};

class UtpOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    UtpOuiServiceClient(asio::io_service& ios, std::string endpoint);

    void start(asio::yield_context) override {}
    void stop() override {}

    GenericStream connect(asio::yield_context, Cancel&) override;

    bool verify_endpoint() const { return (bool)_endpoint; }

    private:
    asio::io_service& _ios;
    boost::optional<asio::ip::udp::endpoint> _endpoint;
};

} // ouiservice namespace
} // ouinet namespace
