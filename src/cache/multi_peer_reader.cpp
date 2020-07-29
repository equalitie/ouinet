#include <asio_utp.hpp>

#include "multi_peer_reader.h"
#include "multi_peer_reader_error.h"
#include "cache_entry.h"
#include "http_sign.h"
#include "hash_list.h"
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
#include "signed_head.h"

using namespace std;
using namespace ouinet;
using namespace cache;

using udp = asio::ip::udp;
namespace bt = bittorrent;
using namespace ouinet::http_response;
using Errc = MultiPeerReaderErrc;

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

class MultiPeerReader::Peer {
public:
    util::intrusive::list_hook _candidate_hook;
    util::intrusive::list_hook _failure_hook;
    util::intrusive::list_hook _good_peer_hook;

    asio::executor _exec;
    string _key;
    const util::Ed25519PublicKey _cache_pk;
    boost::optional<GenericStream> _connection;
    unique_ptr<http_response::Reader> _reader;
    HashList _hash_list;

    // We may receive multiple ChunkBody parts before we receive the next
    // ChunkHdr with which we can verify data integrity. Thus we need to
    // accumulte the body before it's passed forward.
    std::vector<uint8_t> _accumulated_block;
    util::SHA512 _block_hasher;

    // Once we receive ChunkHdr after we've been accumulating data, we send the
    // data and store the ChunkHdr to be sent next.
    boost::optional<ChunkHdr> _postponed_chunk_hdr;

    Peer(asio::executor exec, const string& key, util::Ed25519PublicKey cache_pk) :
        _exec(exec),
        _key(key),
        _cache_pk(cache_pk)
    {
    }

    size_t block_count() const {
        return _hash_list.blocks.size();
    }

    boost::optional<Part>
    read_part(size_t block_id, Cancel c, asio::yield_context yield)
    {
        using OptPart = boost::optional<Part>;

        assert(_reader || _connection);

        sys::error_code ec;

        if (!_reader) {
            bool timed_out = false;

            auto cc = c.connect([&] { if (_connection) _connection->close(); });

            auto wd = watch_dog(_exec, chrono::seconds(10), [&] {
                    if (c) return;
                    timed_out = true;
                    c();
                });

            http::async_write(*_connection, range_request(http::verb::get, block_id, _key), yield[ec]);

            if (c) ec = asio::error::operation_aborted;
            if (timed_out) ec = asio::error::timed_out;
            if (ec) return or_throw<OptPart>(yield, ec);

            auto r = make_unique<http_response::Reader>(move(*_connection));
            _connection = boost::none;

            auto head = r->async_read_part(c, yield[ec]);

            if (c) ec = asio::error::operation_aborted;
            if (timed_out) ec = asio::error::timed_out;
            if (ec) return or_throw<OptPart>(yield, ec);

            if (!head || !head->is_head()) {
                assert(0);
                return or_throw<OptPart>(yield, Errc::expected_head);
            }

            _reader = std::move(r);
        }

        // XXX Add timeout
        OptPart p;

        if (_postponed_chunk_hdr) {
            p = std::move(*_postponed_chunk_hdr);
            _postponed_chunk_hdr = boost::none;
        } else {
            p = _reader->async_read_part(c, yield[ec]);
            if (ec) return or_throw<OptPart>(yield, ec);
        }

        // This may happen when the message has no body
        if (!p) {
            _connection = _reader->release_stream();
            _reader = nullptr;
            return p;
        }

        if (auto chunk_hdr = p->as_chunk_hdr()) {
            if (chunk_hdr->is_last() && block_id + 1 < block_count()) {
                while (p) {
                    // Flush the trailer
                    p = _reader->async_read_part(c, yield[ec]);
                    if (ec) return or_throw<OptPart>(yield, ec);
                    assert(!p || p->is_trailer());
                }
                _connection = _reader->release_stream();
                _reader = nullptr;
                return boost::none;
            }
        }

        if (auto chunk_body = p->as_chunk_body()) {
            while (true) {
                _block_hasher.update(*chunk_body);

                _accumulated_block.insert(_accumulated_block.end(),
                    chunk_body->begin(), chunk_body->end());

                if (chunk_body->remain == 0) {
                    break;
                }

                p = _reader->async_read_part(c, yield[ec]);
                if (ec) return or_throw<OptPart>(yield, ec);

                chunk_body = p->as_chunk_body();

                if (!chunk_body) {
                    assert(0);
                    return or_throw<OptPart>(yield, Errc::expected_chunk_body);
                }
            }

            p = _reader->async_read_part(c, yield[ec]);

            if (!ec && (!p || !p->is_chunk_hdr())) ec = Errc::expected_chunk_hdr;
            if (ec) return or_throw<OptPart>(yield, ec);

            auto digest = _block_hasher.close();
            _block_hasher = {};

            auto current_block = _hash_list.blocks[block_id];

            if (digest != current_block.data_hash) {
                return or_throw<OptPart>(yield, Errc::inconsistent_hash);
            }

            _postponed_chunk_hdr = std::move(*p->as_chunk_hdr());

            // We rewrite whatever chunk extension the peer sent because we
            // already have all the relevant info verified and thus we don't
            // need to re-verify what the user sent again.
            _postponed_chunk_hdr->exts = cache::block_chunk_ext(current_block.chained_hash_signature);

            return Part{ChunkBody(move(_accumulated_block), 0)};
        }

        return p;
    }

    http::request<http::string_body> request(http::verb verb, const string& key)
    {
        auto uri = uri_from_key(_key);
        http::request<http::string_body> rq{verb, uri, 11 /* version */};
        rq.set(http::field::host, "OuinetClient");
        rq.set(http_::protocol_version_hdr, http_::protocol_version_hdr_current);
        rq.set(http::field::user_agent, "Ouinet.Bep5.Client");
        return rq;
    }

    http::request<http::string_body> range_request(http::verb verb, size_t chunk_id, const string& key)
    {
        auto rq = request(verb, key);
        auto bs = _hash_list.signed_head.block_size();
        size_t first = chunk_id * bs;
        size_t last = (bs > 0) ? (first + bs - 1) : first;
        rq.set(http::field::range, util::str("bytes=", first, "-", last));
        return rq;
    }

    void init(
            udp::endpoint ep,
            bt::MainlineDht& dht,
            std::shared_ptr<unsigned> newest_proto_seen,
            Cancel cancel,
            asio::yield_context yield)
    {
        sys::error_code ec;

        Cancel timeout_cancel(cancel);

        auto wd = watch_dog(_exec, chrono::seconds(10), [&] { timeout_cancel(); });

        auto con = connect(_exec, ep, dht, timeout_cancel, yield[ec]);

        if (timeout_cancel) ec = asio::error::timed_out;
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw(yield, ec);

        auto timeout_cancel_con = timeout_cancel.connect([&] { con.close(); });

        http::async_write(con, request(http::verb::propfind, _key), yield[ec]);

        if (timeout_cancel) ec = asio::error::timed_out;
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw(yield, ec);

        http_response::Reader reader(move(con));

        auto hash_list = HashList::load(reader, _cache_pk, timeout_cancel, yield[ec]);

        if (timeout_cancel) ec = asio::error::timed_out;
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw(yield, ec);

        if (!util::http_proto_version_check_trusted(hash_list.signed_head, *newest_proto_seen))
            // The client expects an injection belonging to a supported protocol version,
            // otherwise we just discard this copy.
            return or_throw(yield, asio::error::not_found);

        _hash_list = move(hash_list);
        _connection = reader.release_stream();
    }
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

        ip.first->second = make_unique<Peer>(_exec, _key, _cache_pk);
        Peer* p = ip.first->second.get();

        _candidate_peers.push_back(*p);

        asio::spawn(_exec, [=, dbg_tag = _dbg_tag, c = _lifetime_cancel] (auto y) mutable {
            TRACK_HANDLER();
            sys::error_code ec;

            if (!dbg_tag.empty()) {
                LOG_INFO(dbg_tag, " fetching from: ", ep);
            }

            p->init(ep, *_dht, _newest_proto_seen, c, y[ec]);

            if (!dbg_tag.empty()) {
                LOG_INFO(dbg_tag, " done fetching: ", ep, " "
                        , " ec:", ec.message(), " c:", bool(c));
            }

            if (c) return;

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
            return or_throw<Peer*>(yield, Errc::no_peers, nullptr);
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

    if (!_head_sent) {
        _head_sent = true;
        return http_response::Part{_chosen_peer->_hash_list.signed_head};
    }

    while (_block_id < _chosen_peer->block_count()) {
        auto ret = _chosen_peer->read_part(_block_id, cancel, yield[ec]);
        if (ec) return or_throw<Ret>(yield, ec);
        if (!ret) {
            ++_block_id;
            continue;
        }
        return ret;
    }

    return or_throw<Ret>(yield, ec);
}

bool MultiPeerReader::is_done() const
{
    if (_closed) return true;
    if (!_chosen_peer) return false;
    return _block_id >= _chosen_peer->block_count();
}

bool MultiPeerReader::is_open() const
{
    if (_closed) return false;
    if (!_chosen_peer) return false;
    return _block_id < _chosen_peer->block_count();
}

void MultiPeerReader::close()
{
    _closed = true;
    if (!_chosen_peer) return;
    if (!_chosen_peer->_reader) return;
    _chosen_peer->_reader->close();
}

MultiPeerReader::~MultiPeerReader() {
    _lifetime_cancel();
}

