#pragma once

#include "../multi_utp_server.h"

namespace ouinet {

namespace bittorrent {
    class MainlineDht;
    class Bep5PeriodicAnnouncer;
}

namespace ouiservice {

class Bep5Server : public OuiServiceImplementationServer
{
public:
    Bep5Server( std::shared_ptr<bittorrent::MainlineDht>
              , boost::asio::ssl::context* ssl_context
              , std::string swarm_name);

    void start_listen(asio::yield_context) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context) override;

    ~Bep5Server();

private:
    std::unique_ptr<MultiUtpServer> _multi_utp_server;
    std::unique_ptr<bittorrent::Bep5PeriodicAnnouncer> _announcer;
};

}} // namespaces

