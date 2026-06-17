#pragma once

#include <list>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/optional.hpp>
#include <asio_utp.hpp>

#include "api.h"
#include "../ouiservice.h"

namespace ouinet {
namespace ouiservice {

class OUINET_COMMON_API UtpOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    UtpOuiServiceServer(asio::any_io_executor, asio::ip::udp::endpoint endpoint, util::LogPath);

    [[nodiscard]]
    sys::error_code start_listen(Async) override;

    void stop_listen() override;

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> accept(Async) override;

    ~UtpOuiServiceServer();

    boost::optional<asio::ip::udp::endpoint> local_endpoint() const {
        if (!_udp_multiplexer) return boost::none;
        return _udp_multiplexer->local_endpoint();
    }

    private:
    asio::any_io_executor _ex;
    asio::ip::udp::endpoint _endpoint;
    Cancel _cancel;
    std::unique_ptr<asio_utp::udp_multiplexer> _udp_multiplexer;
    asio::experimental::channel<void(sys::error_code, GenericStream)> _accept_queue;
};

class OUINET_COMMON_API UtpOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    UtpOuiServiceClient( asio::any_io_executor
                       , asio_utp::udp_multiplexer
                       , asio::ip::udp::endpoint remote_endpoint);

    [[nodiscard]]
    sys::error_code start(Async) override;

    void stop() override {}

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> connect(Async) override;

    boost::optional<asio::ip::udp::endpoint> local_endpoint() const {
        return _udp_multiplexer.local_endpoint();
    }

    bool verify_remote_endpoint() const { return bool(_remote_endpoint); }

    private:
    asio::any_io_executor _ex;
    boost::optional<asio::ip::udp::endpoint> _remote_endpoint;
    asio_utp::udp_multiplexer _udp_multiplexer;
};

} // ouiservice namespace
} // ouinet namespace
