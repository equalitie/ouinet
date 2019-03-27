#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/filesystem.hpp>
#include <memory>

#include "../ouiservice.h"
#include "lampshade/liblampshade.h"

namespace ouinet {
namespace ouiservice {

class LampshadeOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    LampshadeOuiServiceServer(asio::io_service& ios, asio::ip::tcp::endpoint endpoint, boost::filesystem::path state_directory);
    LampshadeOuiServiceServer(asio::io_service& ios, asio::ip::tcp::endpoint endpoint, std::string private_key_der, std::string public_key_der);

    void start_listen(asio::yield_context yield) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context yield) override;

    std::string public_key() const;

    private:
    asio::io_service& _ios;
    asio::ip::tcp::endpoint _endpoint;
    std::string _private_key_der;
    std::string _public_key_der;
    std::unique_ptr<lampshade::Listener> _listener;
};

class LampshadeOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    LampshadeOuiServiceClient(asio::io_service& ios, std::string endpoint_string);

    void start(asio::yield_context yield) override;
    void stop() override;

    GenericStream connect(asio::yield_context yield, Signal<void()>& cancel) override;

    bool verify_endpoint() const { return (bool)_endpoint; }

    private:
    asio::io_service& _ios;
    boost::optional<asio::ip::tcp::endpoint> _endpoint;
    std::string _public_key_der;
    std::unique_ptr<lampshade::Dialer> _dialer;
};




} // ouiservice namespace
} // ouinet namespace
