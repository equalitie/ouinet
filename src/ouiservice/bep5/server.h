#pragma once

#include "../multi_utp_server.h"

namespace ouinet {

namespace bittorrent {
    class DhtBase;
    class Bep5PeriodicAnnouncer;
}

namespace ouiservice {

class Bep5Server : public OuiServiceImplementationServer
{
public:
    Bep5Server( std::shared_ptr<bittorrent::DhtBase>
              , boost::asio::ssl::context* ssl_context
              , std::string swarm_name
              , util::LogPath log_path);

    [[nodiscard]]
    sys::error_code start_listen(Async) override;
    void stop_listen() override;

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> accept(Async) override;

    ~Bep5Server();

private:
    std::unique_ptr<MultiUtpServer> _multi_utp_server;
    std::unique_ptr<bittorrent::Bep5PeriodicAnnouncer> _announcer;
};

}} // namespaces
