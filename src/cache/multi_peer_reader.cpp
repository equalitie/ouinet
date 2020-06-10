#include <asio_utp.hpp>

#include "multi_peer_reader.h"
#include "cache_entry.h"
#include "http_sign.h"
#include "../http_util.h"
#include "../session.h"
#include "../util/watch_dog.h"
#include "../util/crypto.h"
#include "../util/intrusive_list.h"
#include "../bittorrent/is_martian.h"
#include "../constants.h"
#include "../util/yield.h"
#include "../util/set_io.h"
#include "../util/condition_variable.h"

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

class MultiPeerReader::Peer {
public:
    util::intrusive::list_hook _candidate_hook;
    util::intrusive::list_hook _failure_hook;
    util::intrusive::list_hook _good_peer_hook;

    Session session;
};

class MultiPeerReader::Peers {
public:
    Peers(asio::executor exec
         , set<udp::endpoint> local_peer_eps
         , util::Ed25519PublicKey cache_pk
         , const std::string& key
         , const std::string& dht_group
         , std::shared_ptr<bittorrent::MainlineDht> dht
         , std::shared_ptr<DhtLookup> dht_lookup
         , std::shared_ptr<unsigned> newest_proto_seen
         , std::string dbg_tag)
        : _exec(exec)
        , _cv(_exec)
        , _cache_pk(move(cache_pk))
        , _local_peer_eps(move(local_peer_eps))
        , _key(move(key))
        , _dht(move(dht))
        , _dht_group(move(dht_group))
        , _dht_lookup(move(dht_lookup))
        , _newest_proto_seen(move(newest_proto_seen))
        , _our_endpoints(_dht->wan_endpoints())
        , _dbg_tag(move(dbg_tag))
    {
        for (auto ep : _local_peer_eps) {
            add_candidate(ep);
        }

        asio::spawn(_exec, [=, dbg_tag = _dbg_tag, c = _lifetime_cancel] (auto y) mutable {
            TRACK_HANDLER();
            sys::error_code ec;

            if (!dbg_tag.empty()) {
                LOG_INFO(dbg_tag, " DHT lookup:");
                LOG_INFO(dbg_tag, "    key:        ", _key);
                LOG_INFO(dbg_tag, "    dht_group:  ", _dht_group);
                LOG_INFO(dbg_tag, "    swarm_name: ", _dht_lookup->swarm_name());
                LOG_INFO(dbg_tag, "    infohash:   ", _dht_lookup->infohash());
            }

            auto dht_eps = _dht_lookup->get(c, y[ec]);

            if (!dbg_tag.empty()) {
                LOG_INFO(dbg_tag, " DHT lookup result ec:\"", ec.message(), "\" eps:", dht_eps);
            }

            if (c) return;

            _dht_lookup.reset();

            if (!ec) {
                for (auto ep : dht_eps) add_candidate(ep);
            }

            _cv.notify();
        });
    }

    void add_candidate(udp::endpoint ep) {
        if (bt::is_martian(ep)) return;
        if (_our_endpoints.count(ep)) return;

        auto ip = _all_peers.insert({ep, unique_ptr<Peer>()});

        if (!ip.second) return; // Already inserted

        ip.first->second = make_unique<Peer>();
        Peer* p = ip.first->second.get();

        _candidate_peers.push_back(*p);

        asio::spawn(_exec, [=, dbg_tag = _dbg_tag, c = _lifetime_cancel] (auto y) mutable {
            TRACK_HANDLER();
            sys::error_code ec;

            if (!dbg_tag.empty()) {
                LOG_INFO(dbg_tag, " fetching from: ", ep);
            }

            auto session = load_from_connection(_exec, _cache_pk, _key, ep, *_dht, _newest_proto_seen, c, y[ec]);

            if (!dbg_tag.empty()) {
                LOG_INFO(dbg_tag, " done fetching: ", ep, " "
                    , " ec:", ec.message(), " c:", bool(c));
            }

            if (c) return;

            p->session = move(session);
            p->_candidate_hook.unlink();

            if (!ec) {
                _good_peers.push_back(*p);
            } else {
                _failed_peers.push_back(*p);
            }

            _cv.notify();
        });
    }

    Peer* choose_peer(Cancel c, asio::yield_context yield)
    {
        auto cc = _lifetime_cancel.connect([&] { c(); });
        sys::error_code ec;

        while (!c && !ec && _good_peers.empty() && (!_candidate_peers.empty() || _dht_lookup))
            _cv.wait(c, yield[ec]);

        if (ec) return or_throw<Peer*>(yield, ec, nullptr);

        if (_good_peers.empty()) {
            // There's no more candidates
            return or_throw<Peer*>(yield, asio::error::host_unreachable, nullptr);
        }

        return &*_good_peers.begin();
    }

    ~Peers() {
        _lifetime_cancel();
    }

private:
    std::map<udp::endpoint, unique_ptr<Peer>> _all_peers;

    util::intrusive::list<Peer, &Peer::_candidate_hook> _candidate_peers;
    util::intrusive::list<Peer, &Peer::_failure_hook>   _failed_peers;
    util::intrusive::list<Peer, &Peer::_good_peer_hook> _good_peers;

    asio::executor _exec;
    ConditionVariable _cv;

    util::Ed25519PublicKey _cache_pk;
    std::set<asio::ip::udp::endpoint> _local_peer_eps;
    std::string _key;
    std::shared_ptr<bittorrent::MainlineDht> _dht;
    std::string _dht_group;
    std::shared_ptr<DhtLookup> _dht_lookup;
    std::shared_ptr<unsigned> _newest_proto_seen;
    set<udp::endpoint> _our_endpoints;
    std::string _dbg_tag;

    Cancel _lifetime_cancel;
};

MultiPeerReader::MultiPeerReader( asio::executor ex
                                , util::Ed25519PublicKey cache_pk
                                , std::set<asio::ip::udp::endpoint> local_peers
                                , std::string key
                                , std::shared_ptr<bittorrent::MainlineDht> dht
                                , std::string dht_group
                                , std::shared_ptr<DhtLookup> dht_lookup
                                , std::shared_ptr<unsigned> newest_proto_seen
                                , const std::string& dbg_tag)
    : _dbg_tag(dbg_tag)
{
    _peers = make_unique<Peers>(ex
                               , local_peers
                               , cache_pk
                               , key
                               , dht_group
                               , dht
                               , dht_lookup
                               , newest_proto_seen
                               , dbg_tag);
}

boost::optional<http_response::Part>
MultiPeerReader::async_read_part(Cancel cancel, asio::yield_context yield)
{
    auto lc = _lifetime_cancel.connect(cancel);

    using Ret = boost::optional<http_response::Part>;

    sys::error_code ec;

    auto dbg_tag = _dbg_tag;
    if (_closed) return boost::none;

    if (!_chosen_peer) {
        _chosen_peer = _peers->choose_peer(cancel, yield[ec]);
        assert(!cancel || ec == asio::error::operation_aborted);
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<Ret>(yield, ec);
        assert(_chosen_peer);
    }

    return _chosen_peer->session.async_read_part(cancel, yield);
}

bool MultiPeerReader::is_done() const
{
    if (_closed) return true;
    if (!_chosen_peer) return false;
    return _chosen_peer->session.is_done();
}

bool MultiPeerReader::is_open() const
{
    if (_closed) return false;
    if (!_chosen_peer) return true;
    return _chosen_peer->session.is_open();
}

void MultiPeerReader::close()
{
    _closed = true;
    if (!_chosen_peer) return;
    _chosen_peer->session.close();
}

MultiPeerReader::~MultiPeerReader() {
    _lifetime_cancel();
}

