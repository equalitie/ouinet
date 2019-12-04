#include "server.h"
#include "../utp.h"
#include "../tls.h"
#include "../../async_sleep.h"
#include "../../bittorrent/dht.h"
#include "../../bittorrent/bep5_announcer.h"
#include "../../bittorrent/is_martian.h"
#include "../../logger.h"
#include "../../util/hash.h"

using namespace std;
using namespace ouinet;
using namespace ouiservice;

namespace bt = bittorrent;

using AbstractServer = OuiServiceImplementationServer;
using udp = asio::ip::udp;
using tcp = asio::ip::tcp;

//////////////////////////////////////////////////////////////////////
// Server

using namespace std::chrono_literals;

struct Bep5Server::State
{
    State( shared_ptr<bt::MainlineDht> dht
         , bt::NodeID infohash
         , unique_ptr<AbstractServer> srv)
        : dht(move(dht))
        , server(move(srv))
        , infohash(infohash)
    {
    }

    void start( util::AsyncQueue<GenericStream>& accept_queue
              , Cancel& outer_cancel
              , asio::yield_context yield)
    {
        announcer = bt::Bep5Announcer(infohash, dht);

        auto ex = dht->get_executor();

        sys::error_code ec;
        server->start_listen(yield[ec]);
        assert(!ec);

        Cancel cancel(outer_cancel);

        asio::spawn(ex, [&, ex, cancel = move(cancel)]
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

    std::shared_ptr<bt::MainlineDht> dht;
    bt::Bep5Announcer announcer;
    std::unique_ptr<AbstractServer> server;
    bt::NodeID infohash;
};

Bep5Server::Bep5Server( shared_ptr<bt::MainlineDht> dht
                      , boost::asio::ssl::context* ssl_context
                      , string swarm_name)
    : _dht(dht)
    , _accept_queue(_dht->get_executor())
{
    assert(_dht);

    auto ex = _dht->get_executor();

    auto endpoints = _dht->local_endpoints();

    if (endpoints.empty()) {
        LOG_ERROR("Bep5Server: DHT has no endpoints!");
    }

    bt::NodeID infohash = util::sha1_digest(swarm_name);
    LOG_INFO("Injector swarm: sha1('", swarm_name, "'): ", infohash.to_hex());

    for (auto ep : endpoints) {
        auto base = make_unique<ouiservice::UtpOuiServiceServer>(ex, ep);
        if (ssl_context) {
            LOG_INFO("Bep5: uTP/TLS Address: ", ep);
            auto tls = make_unique<ouiservice::TlsOuiServiceServer>(ex, move(base), *ssl_context);
            _states.emplace_back(new State(dht, infohash, move(tls)));
        } else {
            LOG_INFO("Bep5: uTP Address: ", ep);
            _states.emplace_back(new State(dht, infohash, move(base)));
        }
    }
}

void Bep5Server::start_listen(asio::yield_context yield)
{
    for (auto& s : _states) {
        sys::error_code ec;
        s->start(_accept_queue, _cancel, yield[ec]);
        if (ec) {
            LOG_ERROR("Bep5Server: Failed to start listen: ", ec.message());
        }
    }
}

void Bep5Server::stop_listen()
{
    _cancel();
    _states.clear();
}

GenericStream Bep5Server::accept(asio::yield_context yield)
{
    sys::error_code ec;
    auto s = _accept_queue.async_pop(_cancel, yield[ec]);
    return or_throw(yield, ec, move(s));
}

Bep5Server::~Bep5Server()
{
    stop_listen();
}
