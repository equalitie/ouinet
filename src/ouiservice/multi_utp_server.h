#pragma once

#include <ouiservice.h>
#include <boost/asio/ssl.hpp>
#include <boost/asio/experimental/channel.hpp>
#include "api.h"

namespace asio_utp {
    class udp_multiplexer;
}

namespace ouinet {
namespace ouiservice {

class OUINET_COMMON_API MultiUtpServer : public OuiServiceImplementationServer
{
private:
    struct State;

public:
    MultiUtpServer( AsioExecutor
                  , std::vector<asio_utp::udp_multiplexer>
                  , boost::asio::ssl::context* ssl_context
                  , util::LogPath);

    [[nodiscard]]
    sys::error_code start_listen(Async) override;
    void stop_listen() override;

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> accept(Async) override;

    ~MultiUtpServer();

private:
    std::list<std::unique_ptr<State>> _states;
    asio::experimental::channel<void(sys::error_code, GenericStream)> _accept_queue;
    Cancel _cancel;
};

} // namespace ouiservice
} // namespace ouinet
