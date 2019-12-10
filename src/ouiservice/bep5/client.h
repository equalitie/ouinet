#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl.hpp>
#include <asio_utp/udp_multiplexer.hpp>

#include "../../ouiservice.h"
#include <random>

namespace ouinet {

namespace bittorrent {
    class MainlineDht;
}

namespace ouiservice {

class Bep5Client : public OuiServiceImplementationClient
{
private:
    using AbstractClient = OuiServiceImplementationClient;
    struct Swarm;

    struct Candidate {
        asio::ip::udp::endpoint endpoint;
        std::shared_ptr<AbstractClient> client;
    };

public:
    Bep5Client( std::shared_ptr<bittorrent::MainlineDht>
              , std::string injector_swarm_name
              , asio::ssl::context*);

    Bep5Client( std::shared_ptr<bittorrent::MainlineDht>
              , std::string injector_swarm_name
              , std::string helpers_swarm_name
              , asio::ssl::context*);

    void start(asio::yield_context) override;
    void stop() override;

    GenericStream connect(asio::yield_context, Cancel&) override;

    ~Bep5Client();

    asio::executor get_executor();

private:
    std::vector<Candidate> get_peers();

private:
    std::shared_ptr<bittorrent::MainlineDht> _dht;

    std::string _injector_swarm_name;
    std::string _helpers_swarm_name;

    std::unique_ptr<Swarm> _injector_swarm;
    std::unique_ptr<Swarm> _helpers_swarm;

    asio::ssl::context* _injector_tls_ctx;
    Cancel _cancel;

    std::mt19937 _random_generator;

    bool _log_debug = false;

    boost::optional<asio::ip::udp::endpoint> _last_working_ep;
};

}} // namespaces
