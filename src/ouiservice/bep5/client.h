#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl.hpp>
#include <asio_utp/udp_multiplexer.hpp>

#include "../../ouiservice.h"

namespace ouinet {

namespace bittorrent {
    class MainlineDht;
}

namespace ouiservice {

class Bep5Client : public OuiServiceImplementationClient
{
private:
    struct Swarm;

public:
    Bep5Client( std::shared_ptr<bittorrent::MainlineDht>
              , std::string injector_swarm_name
              , asio::ssl::context*);

    void start(asio::yield_context) override;
    void stop() override;

    GenericStream connect(asio::yield_context, Cancel&) override;

    ~Bep5Client();

    asio::executor get_executor();

private:
    std::shared_ptr<bittorrent::MainlineDht> _dht;
    std::unique_ptr<Swarm> _injector_swarm;
    std::string _injector_swarm_name;
    asio::ssl::context* _tls_ctx;
    Cancel _cancel;

    //Peers _injectors;

    bool _log_debug = false;
};

}} // namespaces
