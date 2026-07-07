#include <asio_utp/udp_multiplexer.hpp>
#include <ouiservice/utp.h>
#include <ouiservice/tls.h>
#include <async_sleep.h>
#include <logger.h>
#include "multi_utp_server.h"
#include "util/async.h"

namespace ouinet {

using namespace std;
using namespace ouiservice;

using AbstractServer = OuiServiceImplementationServer;
using udp = asio::ip::udp;
using tcp = asio::ip::tcp;

//////////////////////////////////////////////////////////////////////
// Server

using namespace std::chrono_literals;

struct MultiUtpServer::State
{
    State( asio::any_io_executor ex, unique_ptr<AbstractServer> srv)
        : ex(std::move(ex))
        , server(std::move(srv))
    {
    }

    sys::error_code start( asio::experimental::channel<void(sys::error_code, GenericStream)>& accept_queue
              , Cancel& outer_cancel
              , Async yield)
    {
        sys::error_code ec = server->start_listen(yield);
        if (ec) return ec;

        yield.spawn(outer_cancel, [&] (Async yield) mutable {
            while (true) {
                auto con = server->accept(yield);

                if (!con.has_value()) {
                    async_sleep(100ms, yield);
                    continue;
                }

                auto r = accept_queue.async_send(sys::error_code(), std::move(*con), yield);
                if (!r) break;
            }
        });

        return {};
    }

    asio::any_io_executor ex;
    std::unique_ptr<AbstractServer> server;
};

MultiUtpServer::MultiUtpServer( asio::any_io_executor ex
                              , std::vector<asio_utp::udp_multiplexer> sockets
                              , boost::asio::ssl::context* ssl_context
                              , util::LogPath log_path)
    : _accept_queue(ex)
{
    if (sockets.empty()) {
        LOG_ERROR(log_path, " MultiUtpServer: socket set is empty!");
    }

    for (auto& socket : sockets) {
        auto ep = socket.local_endpoint();
        auto base = make_unique<ouiservice::UtpOuiServiceServer>(std::move(socket), log_path);
        if (ssl_context) {
            LOG_INFO(log_path, " Bep5: uTP/TLS Address: ", ep);
            auto tls = make_unique<ouiservice::TlsOuiServiceServer>(ex, std::move(base), *ssl_context);
            _states.emplace_back(new State(ex, std::move(tls)));
        } else {
            LOG_INFO(log_path, " Bep5: uTP Address: ", ep);
            _states.emplace_back(new State(ex, std::move(base)));
        }
    }
}

sys::error_code MultiUtpServer::start_listen(Async yield)
{
    sys::error_code ret_ec;
    for (auto& s : _states) {
        sys::error_code ec = s->start(_accept_queue, _cancel, yield);
        if (ec) {
            LOG_ERROR(yield, " MultiUtpServer: Failed to start listen; ec=", ec);
            if (!ret_ec) ret_ec = ec;
        }
    }
    return ret_ec;
}

void MultiUtpServer::stop_listen()
{
    _cancel();
    _states.clear();
}

std::expected<GenericStream, sys::error_code> MultiUtpServer::accept(Async yield)
{
    auto s = _accept_queue.async_receive(yield);
    if (!s.has_value()) return std::unexpected(s.error());
    return std::move(*s);
}

MultiUtpServer::~MultiUtpServer()
{
    stop_listen();
}

} // namespace
