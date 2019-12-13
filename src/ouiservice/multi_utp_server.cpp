#include "multi_utp_server.h"
#include "../utp.h"
#include "../tls.h"
#include "../../async_sleep.h"
#include "../../logger.h"

using namespace std;
using namespace ouinet;
using namespace ouiservice;

using AbstractServer = OuiServiceImplementationServer;
using udp = asio::ip::udp;
using tcp = asio::ip::tcp;

//////////////////////////////////////////////////////////////////////
// Server

using namespace std::chrono_literals;

struct MultiUtpServer::State
{
    State( asio::executor ex, unique_ptr<AbstractServer> srv)
        : ex(move(ex))
        , server(move(srv))
    {
    }

    void start( util::AsyncQueue<GenericStream>& accept_queue
              , Cancel& outer_cancel
              , asio::yield_context yield)
    {
        sys::error_code ec;
        server->start_listen(yield[ec]);
        assert(!ec);

        Cancel cancel(outer_cancel);

        asio::spawn(ex, [&, cancel = move(cancel)]
                (asio::yield_context yield) mutable {
            while (!cancel) {
                sys::error_code ec;
                auto con = server->accept(yield[ec]);

                if (cancel) break;

                if (ec) {
                    async_sleep(ex, 100ms, cancel, yield);
                    if (cancel) break;
                    continue;
                }

                accept_queue.async_push(move(con), ec, cancel, yield[ec]);
                assert(!cancel && !ec);
            }
        });
    }

    asio::executor ex;
    std::unique_ptr<AbstractServer> server;
};

MultiUtpServer::MultiUtpServer( asio::executor ex
                              , std::set<asio::ip::udp::endpoint> endpoints
                              , boost::asio::ssl::context* ssl_context)
    : _accept_queue(ex)
{
    if (endpoints.empty()) {
        LOG_ERROR("MultiUtpServer: endpoint set is empty!");
    }

    for (auto ep : endpoints) {
        auto base = make_unique<ouiservice::UtpOuiServiceServer>(ex, ep);
        if (ssl_context) {
            LOG_INFO("Bep5: uTP/TLS Address: ", ep);
            auto tls = make_unique<ouiservice::TlsOuiServiceServer>(ex, move(base), *ssl_context);
            _states.emplace_back(new State(ex, move(tls)));
        } else {
            LOG_INFO("Bep5: uTP Address: ", ep);
            _states.emplace_back(new State(ex, move(base)));
        }
    }
}

void MultiUtpServer::start_listen(asio::yield_context yield)
{
    for (auto& s : _states) {
        sys::error_code ec;
        s->start(_accept_queue, _cancel, yield[ec]);
        if (ec) {
            LOG_ERROR("MultiUtpServer: Failed to start listen: ", ec.message());
        }
    }
}

void MultiUtpServer::stop_listen()
{
    _cancel();
    _states.clear();
}

GenericStream MultiUtpServer::accept(asio::yield_context yield)
{
    sys::error_code ec;
    auto s = _accept_queue.async_pop(_cancel, yield[ec]);
    return or_throw(yield, ec, move(s));
}

MultiUtpServer::~MultiUtpServer()
{
    stop_listen();
}
