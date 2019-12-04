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
    UtpOuiServiceServer(const asio::executor&, asio::ip::udp::endpoint endpoint);

    void start_listen(asio::yield_context) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context) override;

    ~UtpOuiServiceServer();

    boost::optional<asio::ip::udp::endpoint> local_endpoint() const {
        if (!_udp_multiplexer) return boost::none;
        return _udp_multiplexer->local_endpoint();
    }

    private:
    asio::executor _ex;
    asio::ip::udp::endpoint _endpoint;
    Cancel _cancel;
    std::unique_ptr<asio_utp::udp_multiplexer> _udp_multiplexer;
    util::AsyncQueue<asio_utp::socket> _accept_queue;
};

class UtpOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    UtpOuiServiceClient( const asio::executor&
                       , asio_utp::udp_multiplexer
                       , std::string remote_endpoint);

    void start(asio::yield_context) override {}
    void stop() override {}

    GenericStream connect(asio::yield_context, Cancel&) override;

    boost::optional<asio::ip::udp::endpoint> local_endpoint() const {
        return _udp_multiplexer.local_endpoint();
    }

    bool verify_remote_endpoint() const { return bool(_remote_endpoint); }

    private:
    asio::executor _ex;
    boost::optional<asio::ip::udp::endpoint> _remote_endpoint;
    asio_utp::udp_multiplexer _udp_multiplexer;
};

} // ouiservice namespace
} // ouinet namespace
