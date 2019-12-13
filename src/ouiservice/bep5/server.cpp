#include "server.h"
#include "../utp.h"
#include "../tls.h"
#include "../../bittorrent/dht.h"
#include "../../bittorrent/bep5_announcer.h"
#include "../../logger.h"
#include "../../util/hash.h"

using namespace std;
using namespace ouinet;
using namespace ouiservice;

namespace bt = bittorrent;

Bep5Server::Bep5Server( shared_ptr<bt::MainlineDht> dht
                      , boost::asio::ssl::context* ssl_context
                      , string swarm_name)
{
    assert(dht);

    auto ex = dht->get_executor();

    auto endpoints = dht->local_endpoints();

    _multi_utp_server = make_unique<MultiUtpServer>(ex, endpoints, ssl_context);

    bt::NodeID infohash = util::sha1_digest(swarm_name);
    LOG_INFO("Injector swarm: sha1('", swarm_name, "'): ", infohash.to_hex());

    _announcer = make_unique<bt::Bep5PeriodicAnnouncer>(infohash, dht);
}

void Bep5Server::start_listen(asio::yield_context yield)
{
    _multi_utp_server->start_listen(yield);
}

void Bep5Server::stop_listen()
{
    _multi_utp_server->stop_listen();

    _multi_utp_server = nullptr;
    _announcer = nullptr;
}

GenericStream Bep5Server::accept(asio::yield_context yield)
{
    return _multi_utp_server->accept(yield);
}

Bep5Server::~Bep5Server()
{
}
