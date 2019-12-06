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
    LampshadeOuiServiceServer(const asio::executor&, asio::ip::tcp::endpoint endpoint, boost::filesystem::path state_directory);
    LampshadeOuiServiceServer(const asio::executor&, asio::ip::tcp::endpoint endpoint, std::string private_key_der, std::string public_key_der);

    void start_listen(asio::yield_context yield) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context yield) override;

    std::string public_key() const;

    private:
    const asio::executor _ex;
    asio::ip::tcp::endpoint _endpoint;
    std::string _private_key_der;
    std::string _public_key_der;
    std::unique_ptr<lampshade::Listener> _listener;
};

class LampshadeOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    LampshadeOuiServiceClient(const asio::executor&, std::string endpoint_string);

    void start(asio::yield_context yield) override;
    void stop() override;

    GenericStream connect(asio::yield_context yield, Signal<void()>& cancel) override;

    bool verify_endpoint() const { return (bool)_endpoint; }

    private:
    const asio::executor _ex;
    boost::optional<asio::ip::tcp::endpoint> _endpoint;
    std::string _public_key_der;
    std::unique_ptr<lampshade::Dialer> _dialer;
};




} // ouiservice namespace
} // ouinet namespace
