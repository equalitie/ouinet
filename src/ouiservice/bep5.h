#pragma once

#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl.hpp>
#include <asio_utp/udp_multiplexer.hpp>
#include <set>
#include <random>

#include "../ouiservice.h"
#include "../util/async_queue.h"

namespace ouinet {

namespace bittorrent {
    class MainlineDht;
    class Bep5Announcer;
}

namespace ouiservice {

class Bep5Server : public OuiServiceImplementationServer
{
private:
    struct State;

public:
    Bep5Server( std::shared_ptr<bittorrent::MainlineDht>
              , boost::asio::ssl::context* ssl_context
              , std::string swarm_name);

    void start_listen(asio::yield_context) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context) override;

    ~Bep5Server();

private:
    std::shared_ptr<bittorrent::MainlineDht> _dht;
    std::list<std::unique_ptr<State>> _states;
    util::AsyncQueue<GenericStream> _accept_queue;
    Cancel _cancel;
    bool _log_debug = false;
};

class Bep5Client : public OuiServiceImplementationClient
{
private:
    struct Bep5Loop;
    struct Client;

    using Clients = std::map< asio::ip::udp::endpoint
                            , std::unique_ptr<Client>
                            >;

public:
    Bep5Client( std::shared_ptr<bittorrent::MainlineDht>
              , std::string swarm_name
              , asio::ssl::context*);

    void start(asio::yield_context) override;
    void stop() override;

    GenericStream connect(asio::yield_context, Cancel&) override;

    ~Bep5Client();

    asio::io_service& get_io_service();

    // Set to true to have the connect function wait until at least one
    // BEP5 DHT resolution has taken place.
    void wait_for_bep5_resolve(bool value);

private:
    void add_injector_endpoints(const std::set<asio::ip::udp::endpoint>&);

    Clients::iterator choose_client();
    unsigned lowest_fail_count() const;
    std::unique_ptr<Client> build_client(const asio::ip::udp::endpoint&);

    boost::optional<asio_utp::udp_multiplexer>
    choose_multiplexer_for(const asio::ip::udp::endpoint&);

private:
    std::shared_ptr<bittorrent::MainlineDht> _dht;
    std::unique_ptr<Bep5Loop> _bep5_loop;
    std::string _swarm_name;
    asio::ssl::context* _tls_ctx;
    Cancel _cancel;

    std::mt19937 _random_gen;

    Clients _clients;

    bool _log_debug = false;
    bool _wait_for_bep5_resolve = false;
};

}} // namespaces
