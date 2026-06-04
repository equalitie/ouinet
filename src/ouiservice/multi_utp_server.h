#pragma once

#include <ouiservice.h>
#include <boost/asio/ssl.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/ip/udp.hpp>
#include <set>
#include "api.h"

namespace ouinet::ouiservice {

class OUINET_COMMON_API MultiUtpServer : public OuiServiceImplementationServer
{
private:
    struct State;

public:
    MultiUtpServer( AsioExecutor
                  , std::set<asio::ip::udp::endpoint>
                  , boost::asio::ssl::context* ssl_context);

    [[nodiscard]]
    sys::error_code start_listen(Async) override;
    void stop_listen() override;

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> accept(Async) override;

    ~MultiUtpServer();

private:
    std::list<std::unique_ptr<State>> _states;
    //util::AsyncQueue<GenericStream> _accept_queue;
    asio::experimental::channel<void(sys::error_code, GenericStream)> _accept_queue;
    Cancel _cancel;
};

} // namespaces
