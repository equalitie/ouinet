#include <asio_utp.hpp>

#include "multi_peer_reader.h"

#include "ouiservice/i2p/session.h"
#include "ouiservice/i2p/tracker_lookup.h"

#include "multi_peer_reader_error.h"
#include "cache_entry.h"
#include "http_sign.h"
#include "../http_util.h"
#include "../session.h"
#include "../util/async.h"
#include "../util/async_job.h"
#include "../util/compat.h"
#include "../util/condition_variable.h"
#include "../util/crypto_stream.h"
#include "../util/debug.h"
#include "../util/intrusive_list.h"
#include "../util/sign.h"
#include "../util/select.h"
#include "../util/set_io.h"
#include "../util/watch_dog.h"
#include "../async_sleep.h"
#include "../constants.h"
#include "../peer_message.h"
#include "signed_head.h"

#include <boost/asio/spawn.hpp>
#include <chrono>
#include <random>

using namespace std;
using namespace ouinet;
using namespace cache;
using namespace std::chrono_literals;

using Clock = std::chrono::steady_clock;

const Clock::duration READ_HEAD_TIMEOUT = 10s;
const Clock::duration READ_CHUNK_BODY_TIMEOUT = 15s;
const Clock::duration READ_CHUNK_HDR_TIMEOUT = 10s;
const Clock::duration READ_TRAILER_TIMEOUT = 10s;
const Clock::duration WRITE_REQUEST_TIMEOUT = 10s;

using udp = asio::ip::udp;
namespace bt = bittorrent;
using namespace ouinet::http_response;
using Errc = MultiPeerReaderErrc;
using OptPart = boost::optional<Part>;

struct MultiPeerReader::Block {
    ChunkBody chunk_body;
    ChunkHdr chunk_hdr;
    boost::optional<Trailer> trailer;
};

static bool same_ipv(const udp::endpoint& ep1, const udp::endpoint& ep2)
{
    return ep1.address().is_v4() == ep2.address().is_v4();
}

static
std::optional<asio_utp::udp_multiplexer>
choose_multiplexer_for( AsioExecutor exec, const udp::endpoint& ep
                      , const set<udp::endpoint>& lan_my_eps)
{
    for (auto& e : lan_my_eps) {
        if (same_ipv(ep, e)) {
            asio_utp::udp_multiplexer m(exec);
            sys::error_code ec;
            m.bind(e, ec);
            assert(!ec);
            return m;
        }
    }

    return std::nullopt;
}

// TODO: For I2P peers, use i2p_client->connect() instead,
// which also returns a GenericStream.
static
std::expected<GenericStream, sys::error_code>
connect( udp::endpoint ep
       , const set<udp::endpoint>& lan_my_eps
       , Async yield)
{
    sys::error_code ec;
    auto exec = yield.get_executor();

    auto opt_m = choose_multiplexer_for(exec, ep, lan_my_eps);

#ifdef __APPLE__
    if (!opt_m) {
        // No local endpoint with matching IP version (IPv4/IPv6) found
        return std::unexpected(asio::error::network_unreachable);
    }
#else
    assert(opt_m);
#endif

    asio_utp::socket s(exec);

    s.bind(*opt_m, ec);
    if (ec) return std::unexpected(ec);

    yield.cancel_slot([&] { s.close(); });
    ec = s.async_connect(ep, yield);
    if (ec) {
        return std::unexpected(ec);
    }

    return GenericStream(move(s));
}

class MultiPeerReader::Peer {
public:
    util::intrusive::list_hook _candidate_hook;
    util::intrusive::list_hook _good_peer_hook;

    AsioExecutor _exec;
    ResourceId _resource_id;
    CryptoStreamKey _resource_key;
    const sign::PublicKey _cache_pk;

    GenericStream _connection;

    HashList _hash_list;
    Cancel _lifetime_cancel;
    util::LogPath _log_path;

    Peer(AsioExecutor exec, const ResourceId& resource_id, const CryptoStreamKey& resource_key, sign::PublicKey cache_pk, util::LogPath log_path) :
        _exec(exec),
        _resource_id(resource_id),
        _resource_key(resource_key),
        _cache_pk(cache_pk),
        _log_path(std::move(log_path))
    {
    }

    ~Peer() {
        _lifetime_cancel();
    }

    const SignedHead& signed_head() const
    {
        return _hash_list.signed_head;
    }


    size_t block_count() const {
        return _hash_list.blocks.size();
    }

    void send_block_request(size_t block_id, Cancel c, asio::yield_context yield)
    {
        if (!_connection.is_open()) return or_throw(yield, asio::error::not_connected);

        sys::error_code ec;

        auto cl = _lifetime_cancel.connect([&] { c(); });
        auto cc = c.connect([&] { if (_connection.is_open()) _connection.close(); });

        Cancel tc(c);
        auto wd = watch_dog(_exec, WRITE_REQUEST_TIMEOUT, [&] { tc(); });

        http::async_write(_connection, range_request(http::verb::get, block_id, _resource_id), yield[ec]);
        fail_on_error_or_timeout(yield, c, ec, wd);
    }

    // May return boost::none and no error if the response has no body (e.g. redirect msg)
    boost::optional<Block> read_block(size_t block_id, Cancel c, asio::yield_context yield)
    {
        using OptBlock = boost::optional<Block>;

        if (!_connection.is_open()) return or_throw<OptBlock>(yield, asio::error::not_connected);

        sys::error_code ec;

        GenericStream stream = determine_incoming_stream(_connection, yield[ec]);
        return_or_throw_on_error(yield, c, ec, OptBlock{});

        auto reader = http_response::Reader(std::move(stream));

        auto cl = _lifetime_cancel.connect([&] { c(); });
        auto cc = c.connect([&] { if (_connection.is_open()) _connection.close(); });

        auto head = reader.timed_async_read_part(READ_HEAD_TIMEOUT, c, yield[ec]);
        return_or_throw_on_error(yield, c, ec, OptBlock{});

        if (!head || !head->is_head()) {
            return or_throw<OptBlock>(yield, Errc::expected_head);
        }

        auto p = reader.timed_async_read_part(READ_CHUNK_HDR_TIMEOUT, c, yield[ec]);
        return_or_throw_on_error(yield, c, ec, OptBlock{});

        // This may happen when the message has no body
        if (!p) {
            return boost::none;
        }

        auto first_chunk_hdr = p->as_chunk_hdr();

        if (!first_chunk_hdr) {
            return or_throw<OptBlock>(yield, Errc::expected_first_chunk_hdr);
        }

        if (first_chunk_hdr->size > http_::response_data_block_max) {
            assert(0 && "Block is too big");
            return or_throw<OptBlock>(yield, Errc::block_is_too_big);
        }

        Block block{{{}, 0},{0, {}}, boost::none};
        util::SHA512 block_hasher;

        if (first_chunk_hdr->size) {
            // Read the block and the chunk header that comes after it.
            while (true) {
                p = reader.timed_async_read_part(READ_CHUNK_BODY_TIMEOUT, c, yield[ec]);
                return_or_throw_on_error(yield, c, ec, OptBlock{});

                auto chunk_body = p->as_chunk_body();
                if (!chunk_body) {
                    assert(0 && "Expected chunk body");
                    return or_throw<OptBlock>(yield, Errc::expected_chunk_body);
                }

                block_hasher.update(*chunk_body);

                if (block.chunk_body.size() + chunk_body->size() > http_::response_data_block_max) {
                    return or_throw<OptBlock>(yield, Errc::block_is_too_big);
                }

                block.chunk_body.insert(block.chunk_body.end(),
                    chunk_body->begin(), chunk_body->end());

                if (chunk_body->remain == 0) {
                    break;
                }
            }

            p = reader.timed_async_read_part(READ_CHUNK_HDR_TIMEOUT, c, yield[ec]);

            ChunkHdr* last_chunk_hdr = p ? p->as_chunk_hdr() : nullptr;

            if (!last_chunk_hdr) ec = Errc::expected_chunk_hdr;
            else if (last_chunk_hdr->size != 0) ec = Errc::expected_no_more_data;
            return_or_throw_on_error(yield, c, ec, OptBlock{});
        }

        // Check block signature
        {
            auto digest = block_hasher.close();

            auto current_block = _hash_list.blocks[block_id];

            if (digest != current_block.data_hash) {
                return or_throw<OptBlock>(yield, Errc::inconsistent_hash);
            }

            // We rewrite whatever chunk extension the peer sent because we
            // already have all the relevant info verified and thus we don't
            // need to re-verify what the user sent again.
            block.chunk_hdr.exts = cache::block_chunk_ext(current_block.chained_hash_signature);
        }

        // Read the trailer (if any), and make sure we're done with this response
        while (true) {
            p = reader.timed_async_read_part(READ_TRAILER_TIMEOUT, c, yield[ec]);
            return_or_throw_on_error(yield, c, ec, OptBlock{});
            if (!p) {
                // We're done with this request
                break;
            }
            auto trailer = p->as_trailer();
            if (trailer) {
                if (block.trailer)
                    return or_throw<OptBlock>(yield, Errc::trailer_received_twice);
                block.trailer = std::move(*trailer);
            } else {
                return or_throw<OptBlock>(yield, Errc::expected_trailer_or_end_of_response);
            }
        }

        return block;
    }

    http::request<http::string_body> request(http::verb verb, const ResourceId& resource_id)
    {
        auto uri = _resource_id.hex_string();
        http::request<http::string_body> rq{verb, uri, 11 /* version */};
        rq.set(http::field::host, "OuinetClient");
        rq.set(http_::protocol_version_hdr, http_::protocol_version_hdr_current);
        rq.set(http::field::user_agent, "Ouinet.Bep5.Client");
        return rq;
    }

    http::request<http::string_body> range_request(http::verb verb, size_t chunk_id, const ResourceId& resource_id)
    {
        auto rq = request(verb, resource_id);
        auto bs = _hash_list.signed_head.block_size();
        size_t first = chunk_id * bs;
        size_t last = (bs > 0) ? (first + bs - 1) : first;
        rq.set(http::field::range, util::str("bytes=", first, "-", last));
        return rq;
    }

    // Common logic for loading and validating hash list from an established connection
    // for both udp::endpoint and i2p destinations
    std::expected<void, sys::error_code>
    download_hash_list(
        GenericStream con,
        std::shared_ptr<unsigned> newest_proto_seen,
        Async yield
    )
    {
        yield.cancel_slot([&] { con.close(); });

        auto result = compat([&](asio::yield_context yield) {
            return http::async_write(con, request(http::verb::propfind, _resource_id), yield);
        })(yield);
        if (!result) {
            return std::unexpected(result.error());
        }

        auto stream = compat([&](asio::yield_context yield) {
            return determine_incoming_stream(con, yield);
        })(yield);
        if (!stream) {
            return std::unexpected(stream.error());
        }

        auto reader = http_response::Reader(std::move(*stream));
        //auto reader = std::make_unique<http_response::Reader>(move(con));

        auto hash_list = compat([&](Cancel cancel, asio::yield_context yield) {
            return HashList::load(reader, _cache_pk, cancel, yield);
        })(yield);
        if (!hash_list) {
            return std::unexpected(hash_list.error());
        }

        if (!util::http_proto_version_check_trusted(hash_list->signed_head, *newest_proto_seen)) {
            // The client expects an injection belonging to a supported protocol version,
            // otherwise we just discard this copy.
            return std::unexpected(asio::error::not_found);
        }

        _hash_list = move(*hash_list);
        _connection = std::move(con);

        return {};
    }

    // Responses may be either plain-text or cypher-text, we read which type it is here
    // and return a generic stream that uses the input connection's reference.
    GenericStream determine_incoming_stream(GenericStream& con, asio::yield_context yield) {
        sys::error_code ec;
        BlobType blob_type = async_read_blob_type(con, yield[ec]);

        if (ec) return or_throw<GenericStream>(yield, ec);

        switch (blob_type) {
            case BlobType::plain_text:
                return StreamRef<GenericStream>(con);
            case BlobType::cypher_text:
                return CryptoStream<StreamRef<GenericStream>>(con, _resource_key);
        }

        // Unreachable, but removes compilation warning
        return or_throw<GenericStream>(yield, make_error_code(PeerRequestError::invalid_blob_type));
    }

};

struct MultiPeerReader::PreFetch {
    using OptBlock = boost::optional<MultiPeerReader::Block>;

    size_t block_id;
    Peer* peer;

    PreFetch(size_t block_id, Peer* peer)
        : block_id(block_id)
        , peer(peer)
    {}

    virtual ~PreFetch() {}

    virtual OptBlock get_block(Cancel&, asio::yield_context) = 0;
};

class MultiPeerReader::Peers {
public:
    Peers(AsioExecutor exec
         , set<udp::endpoint> lan_my_eps
         , set<udp::endpoint> wan_my_eps
         , set<udp::endpoint> lan_peer_eps
         , sign::PublicKey cache_pk
         , const ResourceId& resource_id
         , const CryptoStreamKey& resource_key
         , std::shared_ptr<DhtLookup> peer_lookup
         , std::shared_ptr<unsigned> newest_proto_seen
         , util::LogPath log_path)
        : _exec(exec)
        , _cv(_exec)
        , _cache_pk(move(cache_pk))
        , _lan_peer_eps(move(lan_peer_eps))
        , _lan_my_eps(move(lan_my_eps))
        , _wan_my_eps(move(wan_my_eps))
        , _resource_id(move(resource_id))
        , _resource_key(resource_key)
        , _dht_lookup(move(peer_lookup))
        , _newest_proto_seen(move(newest_proto_seen))
        , _log_path(move(log_path))
        , _random_generator(_random_device())
    {
        if (!_dht_lookup) {
            _cv.notify();
            return;
        }

        if (auto dht_lock = _dht_lookup->get_dht_lock()) {
            for (auto ep : _lan_peer_eps) {
                add_candidate(ep, *dht_lock);
            }
        }

        task::spawn_detached(
            _exec,
            [
                this,
                log_path = _log_path,
                cancel = _lifetime_cancel
            ] (auto y) mutable {
                Async yield(y, std::move(cancel), std::move(log_path));

                auto dht = _dht_lookup->get_dht_lock();
                assert(dht);

                LOG_DEBUG(yield, " Waiting for DHT to be ready...");

                dht->wait_all_ready(yield);

                _wan_my_eps = dht->wan_endpoints();
                LOG_DEBUG(yield, "DHT is ready (lan=", _lan_my_eps, " wan=", _wan_my_eps, "). Looking up peers...");

                // Keep looking up for peers until success or until `this` is destroyed.
                const auto min_sleep = chrono::milliseconds(100);
                const auto max_sleep = chrono::milliseconds(10000);
                auto sleep = min_sleep;

                while (true) {
                    auto peer_eps = _dht_lookup->get(yield);
                    if (peer_eps) {
                        LOG_DEBUG(yield, " Peer lookup successful: ", *peer_eps);

                        if (auto dht = _dht_lookup->get_dht_lock()) {
                            for (auto ep : *peer_eps) add_candidate(ep, *dht);
                        }
                        break;
                    } else {
                        LOG_DEBUG(yield, " Peer lookup failed: ", peer_eps.error(), ". Retry in ", sleep);
                        async_sleep(sleep, yield);
                        sleep = min(2 * sleep, max_sleep);
                    }
                }

                _dht_lookup.reset();
                _cv.notify();
            });
    }

    Peers(AsioExecutor exec
         , set<udp::endpoint> lan_my_eps
         , set<udp::endpoint> lan_peer_eps
         , sign::PublicKey cache_pk
         , const ResourceId& resource_id
         , const CryptoStreamKey& resource_key
         , std::shared_ptr<unsigned> newest_proto_seen
         , util::LogPath log_path)
        : Peers( exec, move(lan_my_eps), {}, move(lan_peer_eps)
               , move(cache_pk), resource_id, resource_key, nullptr
               , move(newest_proto_seen), move(log_path))
    {}

    // Constructor for BEP3 tracker + I2P peers
    // LAN peers some how depends on DHT (lock?) which might have not been initiated
    // so we don't deal with them here.
    Peers(AsioExecutor exec
         , sign::PublicKey cache_pk
         , const ResourceId& resource_id
         , const CryptoStreamKey& resource_key
         , std::shared_ptr<I2pTrackerLookup> i2p_lookup
         , std::shared_ptr<I2pSession> i2p_session
         , std::shared_ptr<unsigned> newest_proto_seen
         , util::LogPath log_path)
        : _exec(exec)
        , _cv(_exec)
        , _cache_pk(move(cache_pk))
        , _resource_id(resource_id)
        , _resource_key(resource_key)
        , _i2p_lookup(move(i2p_lookup))
        , _i2p_session(move(i2p_session))
        , _newest_proto_seen(move(newest_proto_seen))
        , _log_path(move(log_path))
        , _random_generator(_random_device())
    {
        task::spawn_detached(_exec, [this, log_path = _log_path, c = _lifetime_cancel] (auto y) mutable {
            sys::error_code ec;

            auto i2p_dests = _i2p_lookup->get(Async(y, c, log_path));

            if (!i2p_dests.has_value()) {
                LOG_DEBUG(log_path, " BEP3 tracker lookup result; error=", i2p_dests.error());
                return;
            }

            LOG_DEBUG(log_path, " BEP3 tracker lookup result; peers=", i2p_dests->size());

            for (auto& dest : *i2p_dests) add_candidate(dest);

            _i2p_lookup.reset();

            _cv.notify();
        });
    }

    void add_candidate(const I2pAddress& i2p_dest) {
        auto ip = _all_i2p_peers.insert({i2p_dest, unique_ptr<Peer>()});

        if (!ip.second) return; // Already inserted

        ip.first->second = make_unique<Peer>(_exec, _resource_id, _resource_key, _cache_pk, _log_path);
        Peer* peer = ip.first->second.get();

        _candidate_peers.push_back(*peer);

        task::spawn_detached(
            _exec,
            [
                this,
                peer,
                i2p_dest,
                i2p_session = _i2p_session,
                log_path = _log_path,
                cancel = _lifetime_cancel
            ] (auto y) mutable {
                Async yield(y, cancel, log_path);

                LOG_DEBUG(yield.log_path(), " Fetching hash list from I2P: ", i2p_dest);

                std::expected<void, sys::error_code> result;
                uint16_t retry = 10;

                while (retry--) {
                    LOG_DEBUG(yield.log_path(), " BEP3 downloading hash list ", retry, " ", i2p_dest);

                    result = timeout(
                        MultiPeerReader::BEP3_HASH_LIST_TIMEOUT,
                        [&](auto yield) -> std::expected<void, sys::error_code> {
                            auto con = i2p_session->connect(i2p_dest, yield);
                            if (!con) {
                                return std::unexpected(con.error().code());
                            }

                            //TODO: Actually makes the connection works on the server side.
                            //otherwise this code has not been tested.
                            return peer->download_hash_list(std::move(*con), _newest_proto_seen, yield);
                        },
                        yield
                    );

                    if (!result) {
                        async_sleep(5s, yield);
                        continue;
                    }

                    break;
                }

                LOG_DEBUG(log_path, " Done fetching hash list; i2p_dest="
                                  , i2p_dest
                                  , " result=", debug(result));

                peer->_candidate_hook.unlink();

                if (result) {
                    _good_peers.push_back(*peer);
                }

                _cv.notify();
            }
        );
    }

    void add_candidate(udp::endpoint ep, const bittorrent::DhtBase& dht) {
        if (dht.is_martian(ep)) return;
        if (_wan_my_eps.count(ep)) return;

        auto ip = _all_udp_peers.insert({ep, unique_ptr<Peer>()});

        auto peer_log_path = _log_path.tag(util::str(ep));

        if (!ip.second) return; // Already inserted

        ip.first->second = make_unique<Peer>(_exec, _resource_id, _resource_key, _cache_pk, peer_log_path);
        Peer* peer = ip.first->second.get();

        _candidate_peers.push_back(*peer);

        task::spawn_detached(
            _exec,
            [
                this,
                ep,
                peer,
                lan_my_eps = _lan_my_eps,
                log_path = peer_log_path,
                newest_proto_seen = _newest_proto_seen,
                cancel = _lifetime_cancel
            ] (auto y) mutable {
                Async yield(y, cancel, log_path);

                LOG_DEBUG(yield.log_path(), " Fetching hash list");

                auto result = timeout(
                    MultiPeerReader::BEP5_HASH_LIST_TIMEOUT,
                    [&](auto yield) -> std::expected<void, sys::error_code> {
                        auto con = connect(ep, lan_my_eps, yield);

                        if (!con) {
                            return std::unexpected(con.error());
                        }

                        return peer->download_hash_list(std::move(*con), newest_proto_seen, yield);
                    },
                    yield
                );

                LOG_DEBUG( yield.log_path(), " Done fetching hash list; result=", debug(result));

                if (result == std::unexpected(asio::error::timed_out)) {
                    LOG_DEBUG(yield.log_path(), " BEP5 hash list download timed out");
                    return;
                }

                peer->_candidate_hook.unlink();

                if (result) {
                    _good_peers.push_back(*peer);
                }

                _cv.notify();
            }
        );
    }

    bool still_waiting_for_candidates() const {
        if (_dht_lookup || !_candidate_peers.empty()) return true;
        if (_i2p_lookup) return true;
        return false;
    }

    bool has_enough_good_peers() const {
        // TODO: This can be improved to (e.g.) be also a function of time
        // since peer lookup finished.
        return !_good_peers.empty();
    }

    void wait_for_some_peers_to_respond(Cancel c, asio::yield_context yield)
    {
        if (!_good_peers.empty()) return;

        auto cc = _lifetime_cancel.connect([&] { c(); });
        sys::error_code ec;

        while (!c && !ec && !has_enough_good_peers() && still_waiting_for_candidates())
            _cv.wait(c, yield[ec]);

        if (!ec && _good_peers.empty()) ec = Errc::no_peers;

        return or_throw(yield, ec);
    }

    HashList choose_reference_hash_list(Cancel c, asio::yield_context yield)
    {
        sys::error_code ec;

        wait_for_some_peers_to_respond(c, yield[ec]);
        return_or_throw_on_error(yield, c, ec, HashList{});

        Peer* best_peer = nullptr;;

        for (auto& p : _good_peers) {
            if (!best_peer || p.signed_head().more_recent_than(best_peer->signed_head())) {
                best_peer = &p;
            }
        }

        if (!best_peer) return or_throw<HashList>(yield, Errc::no_peers);

        return best_peer->_hash_list;
    }

    Peer* choose_peer_for_block(
            const HashList& reference_hash_list,
            size_t block_id,
            Cancel c,
            asio::yield_context yield)
    {
        sys::error_code ec;

        wait_for_some_peers_to_respond(c, yield[ec]);
        return_or_throw_on_error(yield, c, ec, nullptr);

        std::vector<Peer*> peers;

        auto reference_block = reference_hash_list.get_block(block_id);

        assert(reference_block);
        if (!reference_block) return or_throw<Peer*>(yield, Errc::no_peers, nullptr);

        for (auto& p : _good_peers) {
            auto opt_b = p._hash_list.get_block(block_id);
            if (opt_b && opt_b->data_hash == reference_block->data_hash) {
                peers.push_back(&p);
            }
        }

        if (peers.empty()) return or_throw<Peer*>(yield, Errc::no_peers, nullptr);

        std::uniform_int_distribution<size_t> distrib(0, peers.size() - 1);
        return peers[distrib(_random_generator)];
    }

    ~Peers() {
        _lifetime_cancel();
    }

    void unmark_as_good(Peer& p) {
        assert(p._good_peer_hook.is_linked());
        if (p._good_peer_hook.is_linked()) p._good_peer_hook.unlink();
    }

private:
    // Peers that are in _all_udp_peers/_all_i2p_peers but are not in either
    // _candidate_peers nor _good_peers are considered as failed.
    std::map<udp::endpoint, unique_ptr<Peer>> _all_udp_peers;
    std::map<I2pAddress, unique_ptr<Peer>> _all_i2p_peers;

    util::intrusive::list<Peer, &Peer::_candidate_hook> _candidate_peers;
    util::intrusive::list<Peer, &Peer::_good_peer_hook> _good_peers;

    AsioExecutor _exec;
    ConditionVariable _cv;

    sign::PublicKey _cache_pk;
    std::set<asio::ip::udp::endpoint> _lan_peer_eps;
    std::set<asio::ip::udp::endpoint> _lan_my_eps;
    std::set<asio::ip::udp::endpoint> _wan_my_eps;
    ResourceId _resource_id;
    CryptoStreamKey _resource_key;
    std::shared_ptr<DhtLookup> _dht_lookup;
    std::shared_ptr<I2pTrackerLookup> _i2p_lookup;
    std::shared_ptr<I2pSession> _i2p_session;
    std::shared_ptr<unsigned> _newest_proto_seen;
    util::LogPath _log_path;

    Cancel _lifetime_cancel;

    std::random_device _random_device;
    std::mt19937 _random_generator;
};

MultiPeerReader::MultiPeerReader( AsioExecutor ex
                                , ResourceId resource_id
                                , CryptoStreamKey resource_key
                                , sign::PublicKey cache_pk
                                , std::set<asio::ip::udp::endpoint> lan_peer_eps
                                , std::set<asio::ip::udp::endpoint> lan_my_eps
                                , std::shared_ptr<unsigned> newest_proto_seen
                                , util::LogPath log_path)
    : _executor(ex)
    , _log_path(std::move(log_path))
{
    _peers = make_unique<Peers>(ex
                               , move(lan_my_eps)
                               , move(lan_peer_eps)
                               , move(cache_pk)
                               , move(resource_id)
                               , move(resource_key)
                               , move(newest_proto_seen)
                               , _log_path.tag("Peers"));
}

MultiPeerReader::MultiPeerReader( AsioExecutor ex
                                , ResourceId resource_id
                                , CryptoStreamKey resource_key
                                , sign::PublicKey cache_pk
                                , std::set<asio::ip::udp::endpoint> lan_peer_eps
                                , std::shared_ptr<DhtLookup> peer_lookup
                                , std::shared_ptr<unsigned> newest_proto_seen
                                , util::LogPath log_path)
    : _executor(ex)
    , _log_path(std::move(log_path))
{
    _peers = make_unique<Peers>(ex
                               , peer_lookup->get_dht_lock()->local_endpoints()
                               , std::set<udp::endpoint>{}
                               , move(lan_peer_eps)
                               , move(cache_pk)
                               , move(resource_id)
                               , move(resource_key)
                               , move(peer_lookup)
                               , move(newest_proto_seen)
                               , _log_path.tag("Peers"));
}

MultiPeerReader::MultiPeerReader( AsioExecutor ex
                                , ResourceId resource_id
                                , CryptoStreamKey resource_key
                                , sign::PublicKey cache_pk
                                , std::shared_ptr<I2pTrackerLookup> i2p_lookup
                                , std::shared_ptr<I2pSession> i2p_session
                                , std::shared_ptr<unsigned> newest_proto_seen
                                , util::LogPath log_path)
    : _executor(ex)
    , _log_path(log_path)
{
    _peers = make_unique<Peers>(ex
                               , move(cache_pk)
                               , move(resource_id)
                               , move(resource_key)
                               , move(i2p_lookup)
                               , move(i2p_session)
                               , move(newest_proto_seen)
                               , log_path);
}

struct MultiPeerReader::PreFetchSequential : MultiPeerReader::PreFetch {
    AsyncJob<boost::none_t> job;

    PreFetchSequential(size_t block_id, Peer* peer, AsioExecutor ex)
        : PreFetch(block_id, peer)
        , job(ex)
    {
        job.start([=] (auto& cancel, auto yield) -> boost::none_t {
            sys::error_code ec;
            peer->send_block_request(block_id, cancel, yield[ec]);
            ec = compute_error_code(ec, cancel);
            return or_throw(yield, ec, boost::none);
        });
    }

    OptBlock get_block(Cancel& cancel, asio::yield_context yield) override {
        sys::error_code ec;

        job.wait_for_finish(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, OptBlock{});

        return peer->read_block(block_id, cancel, yield);
    }
};

struct MultiPeerReader::PreFetchParallel : MultiPeerReader::PreFetch {
    using Job = AsyncJob<OptBlock>;
    Job job;

    PreFetchParallel(size_t block_id, Peer* peer, AsioExecutor ex)
        : PreFetch(block_id, peer)
        , job(ex)
    {
        job.start([=] (auto& cancel, auto yield) -> OptBlock {
            sys::error_code ec;
            peer->send_block_request(block_id, cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec, OptBlock{});
            return peer->read_block(block_id, cancel, yield);
        });
    }

    OptBlock get_block(Cancel& cancel, asio::yield_context yield) override {
        sys::error_code ec;

        job.wait_for_finish(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, OptBlock{});

        Job::Result r = std::move(job.result());

        if (r.ec) {
            if (ec) return or_throw<OptBlock>(yield, ec);
        }

        return std::move(r.retval);
    }
};

void MultiPeerReader::unmark_as_good(Peer& peer)
{
    _peers->unmark_as_good(peer);
    if (_pre_fetch && _pre_fetch->peer == &peer) {
        _pre_fetch = nullptr;
    }
}

std::unique_ptr<MultiPeerReader::PreFetch>
MultiPeerReader::new_fetch_job(size_t block_id, Peer* last_peer, Cancel& cancel, asio::yield_context yield)
{
    using R = std::unique_ptr<MultiPeerReader::PreFetch>;

    if (block_id >= _reference_hash_list->blocks.size())
        return nullptr;

    sys::error_code ec;

    Peer* next_peer = _peers->choose_peer_for_block(*_reference_hash_list, block_id, cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, R{});

    PreFetch* pre_fetch;

    if (last_peer == nullptr || next_peer == last_peer) {
        pre_fetch = new PreFetchSequential(block_id, next_peer, _executor);
    } else {
        pre_fetch = new PreFetchParallel(block_id, next_peer, _executor);
    }

    return std::unique_ptr<PreFetch>(pre_fetch);
}

// May return boost::none and no error if the response has no body (e.g. redirect msg)
boost::optional<MultiPeerReader::Block>
MultiPeerReader::fetch_block(size_t block_id, Cancel& cancel, asio::yield_context yield)
{
    //   Q0   Q1   R0   Q2   R1   Q3   R2
    // |----|----|----|----|----|----|----|...

    using OptBlock = boost::optional<MultiPeerReader::Block>;

    sys::error_code ec;

    if (!_pre_fetch) {
        _pre_fetch = new_fetch_job(block_id, nullptr, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, OptBlock{});

        // new_fetch_job should always return non-null if block_id is valid.
        assert(_pre_fetch);
    }

    auto fetch = std::move(_pre_fetch);

    _pre_fetch = new_fetch_job(block_id + 1, fetch->peer, cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, OptBlock{});

    while (true) {
        auto block = fetch->get_block(cancel, yield[ec]);

        if (cancel) {
            return or_throw<OptBlock>(yield, asio::error::operation_aborted);
        }

        if (ec) {
            // Retry with another peer
            ec = {};

            unmark_as_good(*fetch->peer);

            fetch = new_fetch_job(block_id, nullptr, cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec, OptBlock{});

            // new_fetch_job should always return non-null if block_id is valid.
            assert(fetch);

            continue;
        }

        return block;
    }
}

boost::optional<Part>
MultiPeerReader::async_read_part(Cancel cancel, asio::yield_context yield)
{
    using Ret = boost::optional<Part>;

    sys::error_code ec;

    auto lc = _lifetime_cancel.connect([&] { cancel(); });

    if (cancel) return or_throw<Ret>(yield, asio::error::operation_aborted);
    if (_state == State::closed) return or_throw<Ret>(yield, asio::error::bad_descriptor);
    if (_state == State::done) return boost::none;

    auto r = async_read_part_impl(cancel, yield[ec]);
    ec = compute_error_code(ec, cancel);

    if (ec) {
        _state = State::closed;
        _peers = nullptr;
        return or_throw<Ret>(yield, ec);
    } else if (!r) {
        _state = State::done;
        _peers = nullptr;
    }

    return r;
}

boost::optional<Part>
MultiPeerReader::async_read_part_impl(Cancel& cancel, asio::yield_context yield)
{
    sys::error_code ec;

    if (!_reference_hash_list) {
        auto hl = _peers->choose_reference_hash_list(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, OptPart{});
        _reference_hash_list = std::move(hl);
    }

    if (!_head_sent) {
        _head_sent = true;
        return Part{_reference_hash_list->signed_head};
    }

    if (_next_chunk_body) {
        auto p = std::move(*_next_chunk_body);
        _next_chunk_body = boost::none;
        return {{std::move(p)}};
    }

    if (_next_trailer) {
        if (!_last_chunk_hdr_sent) {
            _last_chunk_hdr_sent = true;
            return Part{ChunkHdr(0, std::move(_next_chunk_hdr_ext))};
        }
        auto p = std::move(*_next_trailer);
        _next_trailer = boost::none;
        mark_done();
        return {{std::move(p)}};
    }

    if (_block_id >= _reference_hash_list->blocks.size()) {
        mark_done();
        if (!_last_chunk_hdr_sent) {
            _last_chunk_hdr_sent = true;
            return Part{ChunkHdr(0, std::move(_next_chunk_hdr_ext))};
        }
        return boost::none;
    }

    while (true /* do until successful block retrieval */) {
        auto block = fetch_block(_block_id, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, OptPart{});

        ++_block_id;

        if (!block) {
            mark_done();
            if (!_last_chunk_hdr_sent) {
                _last_chunk_hdr_sent = true;
                return Part{ChunkHdr(0, std::move(_next_chunk_hdr_ext))};
            }
            return boost::none;
        }

        ChunkHdr chunk_hdr{block->chunk_body.size(), std::move(_next_chunk_hdr_ext)};

        _next_chunk_hdr_ext = std::move(block->chunk_hdr.exts);
        _next_chunk_body = std::move(block->chunk_body);

        if (_block_id == _reference_hash_list->blocks.size()) {
            _next_trailer = std::move(block->trailer);
        }

        return {{std::move(chunk_hdr)}};
    }

    assert(0 && "This shouldn't happen");
    return boost::none;
}

void MultiPeerReader::close()
{
    _state = State::closed;
    _peers = nullptr;;
}

void MultiPeerReader::mark_done()
{
    if (_state == State::closed) return;
    _state = State::done;
}

MultiPeerReader::~MultiPeerReader() {
    _lifetime_cancel();
}
