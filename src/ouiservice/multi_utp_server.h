#pragma once

#include "../../ouiservice.h"
#include "../../util/async_queue.h"
#include <boost/asio/ssl.hpp>
#include <boost/asio/ip/udp.hpp>
#include <set>

namespace ouinet {

namespace ouiservice {

class MultiUtpServer : public OuiServiceImplementationServer
{
private:
    struct State;

public:
    MultiUtpServer( asio::executor
                  , std::set<asio::ip::udp::endpoint>
                  , boost::asio::ssl::context* ssl_context);

    void start_listen(asio::yield_context) override;
    void stop_listen() override;

    GenericStream accept(asio::yield_context) override;

    ~MultiUtpServer();

private:
    std::list<std::unique_ptr<State>> _states;
    util::AsyncQueue<GenericStream> _accept_queue;
    Cancel _cancel;
};

}} // namespaces

