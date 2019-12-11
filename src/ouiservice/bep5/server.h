#pragma once

#include <boost/asio/ssl.hpp>

#include "../../ouiservice.h"
#include "../../util/async_queue.h"

namespace ouinet {

namespace bittorrent {
    class MainlineDht;
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
    std::list<std::unique_ptr<State>> _states;
    util::AsyncQueue<GenericStream> _accept_queue;
    Cancel _cancel;
};

}} // namespaces

