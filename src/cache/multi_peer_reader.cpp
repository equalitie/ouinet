#include <asio_utp.hpp>

#include "multi_peer_reader.h"

#include "ouiservice/i2p/session.h"
#include "ouiservice/i2p/tracker_lookup.h"

#include "multi_peer_reader_error.h"
#include "http_sign.h"
#include "../http_util.h"
#include "../session.h"
#include "../util/async.h"
#include "../util/async_job.h"
#include "../util/condition_variable.h"
#include "../util/crypto_stream.h"
#include "../util/debug.h"
#include "../util/intrusive_list.h"
#include "../util/sign.h"
#include "../util/select.h"
#include "../util/watch_dog.h"
#include "../async_sleep.h"
#include "../constants.h"
#include "../peer_message.h"
#include "signed_head.h"

#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <chrono>
#include <expected>
#include <optional>
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
using namespace ouinet::http_response;
using Errc = MultiPeerReaderErrc;
using OptPart = std::optional<Part>;

struct MultiPeerReader::Block {
    ChunkBody chunk_body;
    ChunkHdr chunk_hdr;
    std::optional<Trailer> trailer;
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

    auto cancelled = yield.cancel_slot([&] { s.close(); });
    auto r = s.async_connect(ep, yield);
    if (!r) {
        return std::unexpected(r.error());
    }

    return GenericStream(std::move(s));
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

    std::expected<void, sys::error_code>
    send_block_request(size_t block_id, Async yield)
    {
        if (!_connection.is_open()) {
            return std::unexpected(asio::error::not_connected);
        }

        auto cl = _lifetime_cancel.connect([&] { yield.cancel(); });

        auto e = timeout(WRITE_REQUEST_TIMEOUT, [&] (Async yield) {
            auto cancelled = yield.cancel_slot([&] {
                if (_connection.is_open()) {
                    _connection.close();
                }
            });

            return http::async_write(
                _connection,
                range_request(http::verb::get, block_id, _resource_id),
                yield
            );
        }, yield);
        if (!e) {
            return std::unexpected(e.error());
        }

        return {};
    }

    // May return std::nullopt and no error if the response has no body (e.g. redirect msg)
    std::expected<std::optional<Block>, sys::error_code>
    read_block(size_t block_id, Async yield)
    {
        if (!_connection.is_open()) {
            return std::unexpected(asio::error::not_connected);
        }

        auto stream_e = determine_incoming_stream(_connection, yield);
        if (!stream_e) {
            return std::unexpected(stream_e.error());
        }
        auto reader = http_response::Reader(std::move(*stream_e));

        auto cl = _lifetime_cancel.connect([&] { yield.cancel(); });

        auto head_e = reader.timed_async_read_part(READ_HEAD_TIMEOUT, yield);
        if (!head_e) {
            return std::unexpected(head_e.error());
        }
        auto head = std::move(*head_e);
        if (!head || !head->is_head()) {
            return std::unexpected(Errc::expected_head);
        }

        auto part_e = reader.timed_async_read_part(READ_CHUNK_HDR_TIMEOUT, yield);
        if (!part_e) {
            return std::unexpected(part_e.error());
        }
        auto part = std::move(*part_e);

        // This may happen when the message has no body
        if (!part) {
            return std::nullopt;
        }

        auto first_chunk_hdr = part->as_chunk_hdr();
        if (!first_chunk_hdr) {
            return std::unexpected(Errc::expected_first_chunk_hdr);
        }

        if (first_chunk_hdr->size > http_::response_data_block_max) {
            assert(0 && "Block is too big");
            return std::unexpected(Errc::block_is_too_big);
        }

        Block block{{{}, 0},{0, {}}, std::nullopt};
        util::SHA512 block_hasher;

        if (first_chunk_hdr->size) {
            // Read the block and the chunk header that comes after it.
            while (true) {
                part_e = reader.timed_async_read_part(READ_CHUNK_BODY_TIMEOUT, yield);
                if (!part_e) {
                    return std::unexpected(part_e.error());
                }
                part = std::move(*part_e);

                auto chunk_body = part->as_chunk_body();
                if (!chunk_body) {
                    assert(0 && "Expected chunk body");
                    return std::unexpected(Errc::expected_chunk_body);
                }

                block_hasher.update(*chunk_body);

                if (block.chunk_body.size() + chunk_body->size() > http_::response_data_block_max) {
                    return std::unexpected(Errc::block_is_too_big);
                }

                block.chunk_body.insert(
                    block.chunk_body.end(),
                    chunk_body->begin(),
                    chunk_body->end()
                );

                if (chunk_body->remain == 0) {
                    break;
                }
            }

            part_e = reader.timed_async_read_part(READ_CHUNK_HDR_TIMEOUT, yield);
            if (!part_e) {
                return std::unexpected(part_e.error());
            }
            part = std::move(*part_e);

            ChunkHdr* last_chunk_hdr = part ? part->as_chunk_hdr() : nullptr;
            if (!last_chunk_hdr) {
                return std::unexpected(Errc::expected_chunk_hdr);
            } else if (last_chunk_hdr->size != 0) {
                return std::unexpected(Errc::expected_no_more_data);
            }
        }

        // Check block signature
        {
            auto digest = block_hasher.close();

            auto current_block = _hash_list.blocks[block_id];

            if (digest != current_block.data_hash) {
                return std::unexpected(Errc::inconsistent_hash);
            }

            // We rewrite whatever chunk extension the peer sent because we
            // already have all the relevant info verified and thus we don't
            // need to re-verify what the user sent again.
            block.chunk_hdr.exts = cache::block_chunk_ext(current_block.chained_hash_signature);
        }

        // Read the trailer (if any), and make sure we're done with this response
        while (true) {
            part_e = reader.timed_async_read_part(READ_TRAILER_TIMEOUT, yield);
            if (!part_e) {
                return std::unexpected(part_e.error());
            }

            part = std::move(*part_e);
            if (!part) {
                // We're done with this request
                break;
            }

            auto trailer = part->as_trailer();
            if (trailer) {
                if (block.trailer) {
                    return std::unexpected(Errc::trailer_received_twice);
                }
                block.trailer = std::move(*trailer);
            } else {
                return std::unexpected(Errc::expected_trailer_or_end_of_response);
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
        auto cancelled = yield.cancel_slot([&] { con.close(); });

        auto result = http::async_write(con, request(http::verb::propfind, _resource_id), yield);
        if (!result) {
            return std::unexpected(result.error());
        }

        auto stream = determine_incoming_stream(con, yield);
        if (!stream) {
            return std::unexpected(stream.error());
        }

        auto reader = http_response::Reader(std::move(*stream));

        auto hash_list_e = HashList::load(reader, _cache_pk, yield);
        if (!hash_list_e) {
            return std::unexpected(hash_list_e.error());
        }
        auto hash_list = std::move(*hash_list_e);

        if (!util::http_proto_version_check_trusted(hash_list.signed_head, *newest_proto_seen)) {
            // The client expects an injection belonging to a supported protocol version,
            // otherwise we just discard this copy.
            return std::unexpected(asio::error::not_found);
        }

        _hash_list = std::move(hash_list);
        _connection = std::move(con);

        return {};
    }

    // Responses may be either plain-text or cypher-text, we read which type it is here
    // and return a generic stream that uses the input connection's reference.
    std::expected<GenericStream, sys::error_code>
    determine_incoming_stream(GenericStream& con, Async yield) {
        auto blob_type_e = async_read_blob_type(con, yield);
        if (!blob_type_e) {
            return std::unexpected(blob_type_e.error());
        }
        auto blob_type = std::move(*blob_type_e);

        switch (blob_type) {
            case BlobType::plain_text:
                return StreamRef<GenericStream>(con);
            case BlobType::cypher_text:
                return CryptoStream<StreamRef<GenericStream>>(con, _resource_key);
        }

        // Unreachable, but removes compilation warning
        return std::unexpected(make_error_code(PeerRequestError::invalid_blob_type));
    }
};

struct MultiPeerReader::PreFetch {
    using OptBlock = std::optional<MultiPeerReader::Block>;

    size_t block_id;
    Peer* peer;

    PreFetch(size_t block_id, Peer* peer)
        : block_id(block_id)
        , peer(peer)
    {}

    virtual ~PreFetch() {}

    virtual std::expected<OptBlock, sys::error_code> get_block(Async) = 0;
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
        , _cache_pk(std::move(cache_pk))
        , _lan_peer_eps(std::move(lan_peer_eps))
        , _lan_my_eps(std::move(lan_my_eps))
        , _wan_my_eps(std::move(wan_my_eps))
        , _resource_id(std::move(resource_id))
        , _resource_key(resource_key)
        , _dht_lookup(std::move(peer_lookup))
        , _newest_proto_seen(std::move(newest_proto_seen))
        , _log_path(std::move(log_path))
        , _random_generator(_random_device())
    {
        if (!_dht_lookup) {
            _cv.notify();
            return;
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

                LOG_DEBUG(yield, " Waiting for DHT to get ready...");

                dht->wait_all_ready(yield);

                _wan_my_eps = dht->wan_endpoints();
                LOG_DEBUG(yield, " Waiting for DHT to get ready: done");
                LOG_DEBUG(yield, " Looking up peers...");

                if (auto dht = _dht_lookup->get_dht_lock()) {
                    for (auto ep : _lan_peer_eps) {
                        add_candidate(ep, *dht);
                    }
                }

                // Keep looking up for peers until success or until `this` is destroyed.
                const auto min_sleep = chrono::milliseconds(100);
                const auto max_sleep = chrono::milliseconds(10000);
                auto sleep = min_sleep;

                while (true) {
                    auto peer_eps = _dht_lookup->get(yield);

                    if (peer_eps && !peer_eps->empty()) {
                        LOG_DEBUG(yield, " Found ", peer_eps->size(), " peers");

                        if (auto dht = _dht_lookup->get_dht_lock()) {
                            for (auto ep : *peer_eps) add_candidate(ep, *dht);
                        }

                        break;
                    } else {
                        if (peer_eps) {
                            LOG_DEBUG(yield, " Found 0 peers. Retry in ", sleep);
                        } else {
                            LOG_DEBUG(yield, " Peer lookup failed: ", peer_eps.error(), ". Retry in ", sleep);
                        }

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
        : Peers( exec, std::move(lan_my_eps), {}, std::move(lan_peer_eps)
               , std::move(cache_pk), resource_id, resource_key, nullptr
               , std::move(newest_proto_seen), std::move(log_path))
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
        , _cache_pk(std::move(cache_pk))
        , _resource_id(resource_id)
        , _resource_key(resource_key)
        , _i2p_lookup(std::move(i2p_lookup))
        , _i2p_session(std::move(i2p_session))
        , _newest_proto_seen(std::move(newest_proto_seen))
        , _log_path(std::move(log_path))
        , _random_generator(_random_device())
    {
        task::spawn_detached(_exec, [this, log_path = _log_path, c = _lifetime_cancel] (auto y) mutable {
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
        if (!dht.is_peer_allowed(ep)) return;
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

    std::expected<void, sys::error_code> wait_for_some_peers_to_respond(Async yield)
    {
        if (!_good_peers.empty()) {
            return {};
        }

        auto cc = _lifetime_cancel.connect([&] { yield.cancel(); });

        while (!has_enough_good_peers() && still_waiting_for_candidates()) {
            _cv.wait(yield).value();
        }

        if (_good_peers.empty()) {
            return std::unexpected(Errc::no_peers);
        }

        return {};
    }

    std::expected<HashList, sys::error_code> choose_reference_hash_list(Async yield)
    {
        auto e = wait_for_some_peers_to_respond(yield);
        if (!e) {
            return std::unexpected(e.error());
        }

        Peer* best_peer = nullptr;;

        for (auto& p : _good_peers) {
            if (!best_peer || p.signed_head().more_recent_than(best_peer->signed_head())) {
                best_peer = &p;
            }
        }

        if (!best_peer) {
            return std::unexpected(Errc::no_peers);
        }

        return best_peer->_hash_list;
    }

    std::expected<Peer*, sys::error_code> choose_peer_for_block(
        const HashList& reference_hash_list,
        size_t block_id,
        Async yield
    )
    {
        auto e = wait_for_some_peers_to_respond(yield);
        if (!e) {
            return std::unexpected(e.error());

        }

        std::vector<Peer*> peers;

        auto reference_block = reference_hash_list.get_block(block_id);

        assert(reference_block);
        if (!reference_block) {
            return std::unexpected(Errc::no_peers);
        }

        for (auto& p : _good_peers) {
            auto opt_b = p._hash_list.get_block(block_id);
            if (opt_b && opt_b->data_hash == reference_block->data_hash) {
                peers.push_back(&p);
            }
        }

        if (peers.empty()) {
            return std::unexpected(Errc::no_peers);
        }

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
                               , std::move(lan_my_eps)
                               , std::move(lan_peer_eps)
                               , std::move(cache_pk)
                               , std::move(resource_id)
                               , std::move(resource_key)
                               , std::move(newest_proto_seen)
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
                               , std::move(lan_peer_eps)
                               , std::move(cache_pk)
                               , std::move(resource_id)
                               , std::move(resource_key)
                               , std::move(peer_lookup)
                               , std::move(newest_proto_seen)
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
                               , std::move(cache_pk)
                               , std::move(resource_id)
                               , std::move(resource_key)
                               , std::move(i2p_lookup)
                               , std::move(i2p_session)
                               , std::move(newest_proto_seen)
                               , log_path);
}

struct MultiPeerReader::PreFetchSequential : MultiPeerReader::PreFetch {
    PreFetchSequential(size_t block_id, Peer* peer, AsioExecutor ex)
        : PreFetch(block_id, peer)
    {}

    std::expected<OptBlock, sys::error_code>
    get_block(Async yield) override {
        return peer->send_block_request(block_id, yield)
            .and_then([&] { return peer->read_block(block_id, yield); });
    }
};

struct MultiPeerReader::PreFetchParallel : MultiPeerReader::PreFetch {
    using Job = AsyncJob<OptBlock>;
    Job job;

    PreFetchParallel(size_t block_id, Peer* peer, AsioExecutor ex)
        : PreFetch(block_id, peer)
        , job(ex)
    {
        job.start([=] (Async yield) -> std::expected<OptBlock, sys::error_code> {
            auto e = peer->send_block_request(block_id, yield);
            if (!e) {
                return std::unexpected(e.error());
            }

            return peer->read_block(block_id, yield);
        });
    }

    std::expected<OptBlock, sys::error_code> get_block(Async yield) override {
        job.wait_for_finish(yield);
        return std::move(job.result());
    }
};

void MultiPeerReader::unmark_as_good(Peer& peer)
{
    _peers->unmark_as_good(peer);
    if (_pre_fetch && _pre_fetch->peer == &peer) {
        _pre_fetch = nullptr;
    }
}

std::expected<std::unique_ptr<MultiPeerReader::PreFetch>, sys::error_code>
MultiPeerReader::new_fetch_job(size_t block_id, Peer* last_peer, Async yield)
{
    if (block_id >= _reference_hash_list->blocks.size()) {
        return nullptr;
    }

    auto next_peer_e = _peers->choose_peer_for_block(*_reference_hash_list, block_id, yield);
    if (!next_peer_e) {
        return std::unexpected(next_peer_e.error());
    }
    auto next_peer = std::move(*next_peer_e);

    PreFetch* pre_fetch;

    if (last_peer == nullptr || next_peer == last_peer) {
        pre_fetch = new PreFetchSequential(block_id, next_peer, _executor);
    } else {
        pre_fetch = new PreFetchParallel(block_id, next_peer, _executor);
    }

    return std::unique_ptr<PreFetch>(pre_fetch);
}

// May return std::nullopt and no error if the response has no body (e.g. redirect msg)
std::expected<std::optional<MultiPeerReader::Block>, sys::error_code>
MultiPeerReader::fetch_block(size_t block_id, Async yield)
{
    //   Q0   Q1   R0   Q2   R1   Q3   R2
    // |----|----|----|----|----|----|----|...

    if (!_pre_fetch) {
        auto e = new_fetch_job(block_id, nullptr, yield);
        if (!e) {
            return std::unexpected(e.error());
        }
        _pre_fetch = std::move(*e);

        // new_fetch_job should always return non-null if block_id is valid.
        assert(_pre_fetch);
    }

    auto fetch = std::move(_pre_fetch);

    auto e = new_fetch_job(block_id + 1, fetch->peer, yield);
    if (!e) {
        return std::unexpected(e.error());
    }
    _pre_fetch = std::move(*e);

    while (true) {
        auto block_e = fetch->get_block(yield);
        if (!block_e) {
            // Retry with another peer
            unmark_as_good(*fetch->peer);

            auto fetch_e = new_fetch_job(block_id, nullptr, yield);
            if (!fetch_e) {
                return std::unexpected(fetch_e.error());
            }
            fetch = std::move(*fetch_e);

            // new_fetch_job should always return non-null if block_id is valid.
            assert(fetch);

            continue;
        }

        return *block_e;
    }
}

std::expected<std::optional<Part>, sys::error_code>
MultiPeerReader::async_read_part(Async yield)
{
    auto lc = _lifetime_cancel.connect([&] { yield.cancel(); });

    if (_state == State::closed) return std::unexpected(asio::error::bad_descriptor);
    if (_state == State::done) return std::nullopt;

    auto result = async_read_part_impl(yield);
    if (!result) {
        _state = State::closed;
        _peers = nullptr;
        return std::unexpected(result.error());
    }

    if (!*result) {
        _state = State::done;
        _peers = nullptr;
    }

    return *result;
}

std::expected<std::optional<Part>, sys::error_code>
MultiPeerReader::async_read_part_impl(Async yield)
{
    if (!_reference_hash_list) {
        auto hl = _peers->choose_reference_hash_list(yield);
        if (!hl) {
            return std::unexpected(hl.error());
        }

        _reference_hash_list = std::move(*hl);
    }

    if (!_head_sent) {
        _head_sent = true;
        return Part{_reference_hash_list->signed_head};
    }

    if (_next_chunk_body) {
        auto p = std::move(*_next_chunk_body);
        _next_chunk_body = std::nullopt;
        return std::move(p);
    }

    if (_next_trailer) {
        if (!_last_chunk_hdr_sent) {
            _last_chunk_hdr_sent = true;
            return Part{ChunkHdr(0, std::move(_next_chunk_hdr_ext))};
        }
        auto p = std::move(*_next_trailer);
        _next_trailer = std::nullopt;
        mark_done();
        return std::move(p);
    }

    if (_block_id >= _reference_hash_list->blocks.size()) {
        mark_done();
        if (!_last_chunk_hdr_sent) {
            _last_chunk_hdr_sent = true;
            return Part{ChunkHdr(0, std::move(_next_chunk_hdr_ext))};
        }
        return std::nullopt;
    }

    while (true /* do until successful block retrieval */) {
        auto block_e = fetch_block(_block_id, yield);
        if (!block_e) {
            return std::unexpected(block_e.error());
        }
        auto block = std::move(*block_e);

        ++_block_id;

        if (!block) {
            mark_done();
            if (!_last_chunk_hdr_sent) {
                _last_chunk_hdr_sent = true;
                return Part{ChunkHdr(0, std::move(_next_chunk_hdr_ext))};
            }
            return std::nullopt;
        }

        ChunkHdr chunk_hdr{block->chunk_body.size(), std::move(_next_chunk_hdr_ext)};

        _next_chunk_hdr_ext = std::move(block->chunk_hdr.exts);
        _next_chunk_body = std::move(block->chunk_body);

        if (_block_id == _reference_hash_list->blocks.size()) {
            _next_trailer = std::move(block->trailer);
        }

        return std::move(chunk_hdr);
    }

    assert(0 && "This shouldn't happen");
    return std::nullopt;
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
