#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/optional.hpp>
#include <asio_utp/udp_multiplexer.hpp>

#include "../../ouiservice.h"
#include <random>

namespace ouinet {

class Async;

namespace bittorrent {
    class DhtBase;
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
    enum class SwarmType {
        injector,
        helper
    };

    friend std::ostream& operator<<(std::ostream& os, SwarmType t) {
        switch (t) {
            case SwarmType::injector: return os << "injector";
            case SwarmType::helper: return os << "helper";
        }
        assert(false);
        return os << "?";
    }

    using AbstractClient = OuiServiceImplementationClient;
    struct Swarm;
    class InjectorPinger;

    struct Candidate {
        asio::ip::udp::endpoint endpoint;
        std::shared_ptr<AbstractClient> client;
        SwarmType swarm_type;

        friend std::ostream& operator<<(std::ostream& os, const Candidate& c) {
            return os << "Candidate{ endpoint:" << c.endpoint << ", client:" << c.client.get() << ", type:" << c.swarm_type << "}";
        }
    };

    struct Candidates;
    friend struct Candidates;

public:
    Bep5Client( std::shared_ptr<bittorrent::DhtBase>
              , std::string injector_swarm_name
              , asio::ssl::context*
              , Target targets
              , const util::LogPath& log_path);

    Bep5Client( std::shared_ptr<bittorrent::DhtBase>
              , std::string injector_swarm_name
              , std::string helpers_swarm_name
              , bool helper_announcement_enabled
              , asio::ssl::context*
              , Target targets
              , const util::LogPath& log_path);

    [[nodiscard]]
    sys::error_code start(Async) override;
    void stop() override;
    size_t injector_candidates_n() const noexcept;

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> connect(Async) override;

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> connect(Async, bool use_tls, Target);

    ~Bep5Client();

    AsioExecutor get_executor();

private:
    void status_loop(Async);

    std::expected<GenericStream, sys::error_code> connect_single(AbstractClient&, bool use_tls, Async);

private:
    std::shared_ptr<bittorrent::DhtBase> _dht;

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
