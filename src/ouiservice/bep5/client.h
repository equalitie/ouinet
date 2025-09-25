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
public:
    enum Target : uint8_t { none = 0, helpers = 1, injectors = 2 };

    friend Target operator|(Target t1, Target t2) {
        return static_cast<Target>( static_cast<uint8_t>(t1)
                                  | static_cast<uint8_t>(t2));
    }

private:
    using AbstractClient = OuiServiceImplementationClient;
    struct Swarm;
    class InjectorPinger;

    struct Candidate {
        asio::ip::udp::endpoint endpoint;
        std::shared_ptr<AbstractClient> client;
        Target target;
    };

public:
    Bep5Client( std::shared_ptr<bittorrent::MainlineDht>
              , std::string injector_swarm_name
              , asio::ssl::context*
              , Target targets = helpers | injectors);

    Bep5Client( std::shared_ptr<bittorrent::MainlineDht>
              , std::string injector_swarm_name
              , std::string helpers_swarm_name
              , bool helper_announcement_enabled
              , asio::ssl::context*
              , Target targets = helpers | injectors);

    void start(asio::yield_context) override;
    void stop() override;
    bool is_ready() const noexcept;

    GenericStream connect(asio::yield_context, Cancel&) override;
    GenericStream connect(asio::yield_context, Cancel&, bool tls, Target);

    ~Bep5Client();

    AsioExecutor get_executor();

private:
    void status_loop(asio::yield_context);
    std::vector<Candidate> get_peers(Target);

    GenericStream connect_single(AbstractClient&, bool tls, Cancel&, asio::yield_context);

private:
    std::shared_ptr<bittorrent::MainlineDht> _dht;

    std::string _injector_swarm_name;
    std::string _helpers_swarm_name;
    bool _helper_announcement_enabled;

    std::shared_ptr<Swarm> _injector_swarm;
    std::unique_ptr<Swarm> _helpers_swarm;

    std::unique_ptr<InjectorPinger> _injector_pinger;

    asio::ssl::context* _injector_tls_ctx;
    Cancel _cancel;

    std::mt19937 _random_generator;

    static const bool _log_debug = false;  // for development testing only

    boost::optional<asio::ip::udp::endpoint> _last_working_ep;

    Target _default_targets;
};

}} // namespaces
