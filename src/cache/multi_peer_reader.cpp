#include <asio_utp.hpp>

#include "multi_peer_reader.h"
#include "cache_entry.h"
#include "http_sign.h"
#include "../http_util.h"
#include "../session.h"
#include "../util/watch_dog.h"
#include "../util/crypto.h"
#include "../bittorrent/is_martian.h"
#include "../constants.h"
#include "../util/yield.h"
#include "../util/set_io.h"

using namespace std;
using namespace ouinet;
using namespace cache;

using udp = asio::ip::udp;
namespace bt = bittorrent;

static bool same_ipv(const udp::endpoint& ep1, const udp::endpoint& ep2)
{
    return ep1.address().is_v4() == ep2.address().is_v4();
}

static
boost::optional<asio_utp::udp_multiplexer>
choose_multiplexer_for(asio::executor exec, const udp::endpoint& ep, bt::MainlineDht& dht)
{
    auto eps = dht.local_endpoints();

    for (auto& e : eps) {
        if (same_ipv(ep, e)) {
            asio_utp::udp_multiplexer m(exec);
            sys::error_code ec;
            m.bind(e, ec);
            assert(!ec);
            return m;
        }
    }

    return boost::none;
}

static
GenericStream connect( asio::executor exec
                     , udp::endpoint ep
                     , bt::MainlineDht& dht
                     , Cancel cancel
                     , asio::yield_context yield)
{
    sys::error_code ec;
    auto opt_m = choose_multiplexer_for(exec, ep, dht);
    assert(opt_m);
    asio_utp::socket s(exec);
    s.bind(*opt_m, ec);
    if (ec) return or_throw<GenericStream>(yield, ec);
    auto cancel_con = cancel.connect([&] { s.close(); });
    s.async_connect(ep, yield[ec]);
    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<GenericStream>(yield, ec);
    return GenericStream(move(s));
}

static
Session load_from_connection( asio::executor exec
                            , const util::Ed25519PublicKey& cache_pk
                            , const string& key
                            , udp::endpoint ep
                            , bt::MainlineDht& dht
                            , std::shared_ptr<unsigned> newest_proto_seen
                            , Cancel cancel
                            , asio::yield_context yield)
{
    sys::error_code ec;

    Cancel timeout_cancel(cancel);

    WatchDog wd(exec, chrono::seconds(10), [&] { timeout_cancel(); });

    auto con = connect(exec, ep, dht, timeout_cancel, yield[ec]);

    if (timeout_cancel) ec = asio::error::timed_out;
    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<Session>(yield, ec);

    auto uri = uri_from_key(key);

    http::request<http::string_body> rq{http::verb::get, uri, 11 /* version */};
    rq.set(http::field::host, "dummy_host");
    rq.set(http_::protocol_version_hdr, http_::protocol_version_hdr_current);
    rq.set(http::field::user_agent, "Ouinet.Bep5.Client");

    auto timeout_cancel_con = timeout_cancel.connect([&] { con.close(); });

    http::async_write(con, rq, yield[ec]);

    if (timeout_cancel) ec = asio::error::timed_out;
    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<Session>(yield, ec);

    Session::reader_uptr vfy_reader = make_unique<cache::VerifyingReader>(move(con), cache_pk);
    auto session = Session::create(move(vfy_reader), timeout_cancel, yield[ec]);

    if (timeout_cancel) ec = asio::error::timed_out;
    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<Session>(yield, ec);

    if ( !ec
        && !util::http_proto_version_check_trusted(session.response_header(), *newest_proto_seen))
        // The client expects an injection belonging to a supported protocol version,
        // otherwise we just discard this copy.
        ec = asio::error::not_found;

    if (!ec) session.response_header().set( http_::response_source_hdr  // for agent
                                          , http_::response_source_hdr_dist_cache);
    return or_throw(yield, ec, move(session));
}

unique_ptr<util::AsyncGenerator<MultiPeerReader::Connection>>
MultiPeerReader::make_connection_generator( asio::executor exec
                                          , set<udp::endpoint> local_peers
                                          , util::Ed25519PublicKey cache_pk
                                          , const std::string& key
                                          , const std::string& dht_group
                                          , std::shared_ptr<bittorrent::MainlineDht> dht
                                          , std::shared_ptr<DhtLookup> dht_lookup
                                          , std::shared_ptr<unsigned> newest_proto_seen
                                          , const std::string& dbg_tag)
{
    using Ret = util::AsyncGenerator<Connection>;

    set<udp::endpoint> eps = move(local_peers);

    if (!dbg_tag.empty()) {
        LOG_INFO(dbg_tag, " local peers:", eps);
    }

    return make_unique<Ret>(exec,
    [=, eps = move(eps)]
    (auto& q, auto c, auto y) mutable {
        WaitCondition wc(exec);
        set<udp::endpoint> our_endpoints = dht->wan_endpoints();

        auto fetch = [exec, newest_proto_seen, &cache_pk, dht, &dbg_tag, &wc, &c, &key, &q, &our_endpoints] (udp::endpoint ep) {
            if (bt::is_martian(ep)) return;
            if (our_endpoints.count(ep)) return;

            asio::spawn(exec, [exec, newest_proto_seen, &cache_pk, dht, &dbg_tag, &q, &c, &key, ep, lock = wc.lock()] (auto y) {
                TRACK_HANDLER();
                sys::error_code ec;

                if (!dbg_tag.empty()) {
                    LOG_INFO(dbg_tag, " fetching from: ", ep);
                }

                auto session = load_from_connection(exec, cache_pk, key, ep, *dht, newest_proto_seen, c, y[ec]);

                if (!dbg_tag.empty()) {
                    LOG_INFO(dbg_tag, " done fetching: ", ep, " "
                        , " ec:", ec.message(), " c:", bool(c));
                }

                if (ec || c) return;

                q.push_back(Connection{ep, move(session)});
            });
        };

        for (auto& ep : eps) {
            fetch(ep);
        }

        //bt::NodeID infohash = util::sha1_digest(swarm_name);

        if (!dbg_tag.empty()) {
            LOG_INFO(dbg_tag, " DHT lookup:");
            LOG_INFO(dbg_tag, "    key:        ", key);
            LOG_INFO(dbg_tag, "    dht_group:  ", dht_group);
            //LOG_INFO(dbg_tag, "    swarm_name: ", swarm_name);
            //LOG_INFO(dbg_tag, "    infohash:   ", infohash);
        }

        sys::error_code ec;
        auto dht_eps = dht_lookup->get(c, y[ec]);

        if (c) ec = asio::error::operation_aborted;

        if (!dbg_tag.empty()) {
            LOG_INFO(dbg_tag, " DHT BEP5 lookup result ec:", ec.message(),
                      " eps:", eps);
        }

        if (!ec) {
            for (auto ep : dht_eps) {
                if (eps.count(ep)) continue;
                fetch(ep);
            }
        }

        ec = {};
        wc.wait(y[ec]);

        if (c) return or_throw(y, asio::error::operation_aborted);
    });
}


MultiPeerReader::MultiPeerReader( asio::executor ex
                                , util::Ed25519PublicKey cache_pk
                                , std::set<asio::ip::udp::endpoint> local_peers
                                , std::string key
                                , std::shared_ptr<bittorrent::MainlineDht> dht
                                , std::string dht_group
                                , std::shared_ptr<DhtLookup> dht_lookup
                                , std::shared_ptr<unsigned> newest_proto_seen
                                , const std::string& dbg_tag)
    : _exec(ex)
    , _dht(std::move(dht))
    , _local_peers(std::move(local_peers))
    , _key(std::move(key))
    , _dht_group(std::move(dht_group))
    , _dht_lookup(std::move(dht_lookup))
    , _newest_proto_seen(std::move(newest_proto_seen))
{
    _connection_generator = make_connection_generator
        ( _exec
        , _local_peers
        , cache_pk
        , _key
        , _dht_group
        , _dht
        , _dht_lookup
        , _newest_proto_seen
        , (dbg_tag.empty() ? dbg_tag : (dbg_tag + "/con_generator")));
}

boost::optional<http_response::Part>
MultiPeerReader::async_read_part(Cancel cancel, asio::yield_context yield)
{
    auto lc = _lifetime_cancel.connect(cancel);

    using Ret = boost::optional<http_response::Part>;

    sys::error_code ec;

    if (_closed) return boost::none;

    if (!_chosen_connection) {
        _chosen_connection = _connection_generator->async_get_value(cancel, yield[ec]);
        assert(!cancel || ec == asio::error::operation_aborted);
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<Ret>(yield, ec);

        if (!_chosen_connection) {
            _closed = true;
            return or_throw<Ret>(yield, asio::error::host_unreachable);
        }
    }

    return _chosen_connection->session.async_read_part(cancel, yield);
}

bool MultiPeerReader::is_done() const
{
    if (_closed) return true;
    if (!_chosen_connection) return false;
    return _chosen_connection->session.is_done();
}

bool MultiPeerReader::is_open() const
{
    if (_closed) return false;
    if (!_chosen_connection) return true;
    return _chosen_connection->session.is_open();
}

void MultiPeerReader::close()
{
    _closed = true;
    if (!_chosen_connection) return;
    _chosen_connection->session.close();
}
