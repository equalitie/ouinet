#include <asio_utp.hpp>

#include "multi_peer_reader.h"
#ifdef __EXPERIMENTAL__
#include <ouiservice/i2p/client.h>
#include <ouiservice/i2p/service.h>
#endif
#include "multi_peer_reader_error.h"
#include "cache_entry.h"
#include "http_sign.h"
#include "../http_util.h"
#include "../session.h"
#include "../util/watch_dog.h"
#include "../util/sign.h"
#include "../util/crypto_stream.h"
#include "../util/intrusive_list.h"
#include "../util/exponential_backoff.h"
#include "../constants.h"
#include "../util/set_io.h"
#include "../util/async_job.h"
#include "../util/condition_variable.h"
#include "../peer_message.h"
#include "signed_head.h"

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
boost::optional<asio_utp::udp_multiplexer>
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

    return boost::none;
}

// TODO: For I2P peers, use i2p_client->connect() instead,
// which also returns a GenericStream.
static
GenericStream connect( AsioExecutor exec
                     , udp::endpoint ep
                     , const set<udp::endpoint>& lan_my_eps
                     , Cancel cancel
                     , asio::yield_context yield)
{
    sys::error_code ec;
    auto opt_m = choose_multiplexer_for(exec, ep, lan_my_eps);

#ifdef __APPLE__
    if (!opt_m) {
        // No local endpoint with matching IP version (IPv4/IPv6) found
        return or_throw<GenericStream>(yield, asio::error::network_unreachable);
    }
#else
    assert(opt_m);
#endif

    asio_utp::socket s(exec);
    s.bind(*opt_m, ec);
    if (ec) return or_throw<GenericStream>(yield, ec);
    auto cancel_con = cancel.connect([&] { s.close(); });
    s.async_connect(ep, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, GenericStream{});
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

#ifdef __EXPERIMENTAL__
    // We need to keep the i2p LocalDestination alive for as long as
    // `_connection` may be written to, because the client destroys its
    // connections when it dies.
    // Otherwise, for example, the per-peer i2p_client that
    // is created in  add_candidate_i2p and gets destroyed as soon as the spawn
    // coroutine exits — even though `_connection` still references its tunnel
    // and if cause segdev if the write happens after
    // the coroutine exited.    
    std::unique_ptr<ouiservice::i2poui::Client> _i2p_client;
#endif

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

        LOG_DEBUG("read_block[", block_id, "]: got first chunk_hdr size=",
                  first_chunk_hdr->size);

        Block block{{{}, 0},{0, {}}, boost::none};
        util::SHA512 block_hasher;

        if (first_chunk_hdr->size) {
            // Read the block and the chunk header that comes after it.
            size_t chunk_body_iter = 0;
            while (true) {
                LOG_DEBUG("read_block[", block_id, "]: awaiting chunk_body iter=",
                          chunk_body_iter, " accumulated=", block.chunk_body.size());
                p = reader.timed_async_read_part(READ_CHUNK_BODY_TIMEOUT, c, yield[ec]);
                if (ec) {
                    LOG_DEBUG("read_block[", block_id,
                              "]: chunk_body read failed on iter=", chunk_body_iter,
                              " accumulated=", block.chunk_body.size(), " ec=", ec);
                }
                return_or_throw_on_error(yield, c, ec, OptBlock{});

                auto chunk_body = p->as_chunk_body();
                if (!chunk_body) {
                    LOG_DEBUG("read_block[", block_id,
                              "]: expected chunk_body but got a different part on iter=",
                              chunk_body_iter);
                    assert(0 && "Expected chunk body");
                    return or_throw<OptBlock>(yield, Errc::expected_chunk_body);
                }

                LOG_DEBUG("read_block[", block_id,
                          "]: got chunk_body piece iter=", chunk_body_iter,
                          " size=", chunk_body->size(),
                          " remain=", chunk_body->remain);

                block_hasher.update(*chunk_body);

                if (block.chunk_body.size() + chunk_body->size() > http_::response_data_block_max) {
                    return or_throw<OptBlock>(yield, Errc::block_is_too_big);
                }

                block.chunk_body.insert(block.chunk_body.end(),
                    chunk_body->begin(), chunk_body->end());

                if (chunk_body->remain == 0) {
                    LOG_DEBUG("read_block[", block_id,
                              "]: chunk_body loop done after iter=", chunk_body_iter,
                              " total=", block.chunk_body.size());
                    break;
                }
                ++chunk_body_iter;
            }

            // Now expect the terminator chunk header (size == 0).
            LOG_DEBUG("read_block[", block_id,
                      "]: chunk body complete — awaiting terminator chunk_hdr");
            p = reader.timed_async_read_part(READ_CHUNK_HDR_TIMEOUT, c, yield[ec]);
            if (ec) {
                LOG_DEBUG("read_block[", block_id,
                          "]: terminator chunk_hdr read failed ec=", ec);
            }

            ChunkHdr* last_chunk_hdr = p ? p->as_chunk_hdr() : nullptr;

            if (!last_chunk_hdr) {
                LOG_DEBUG("read_block[", block_id,
                          "]: expected terminator chunk_hdr, got nullptr/other part");
                ec = Errc::expected_chunk_hdr;
            }
            else if (last_chunk_hdr->size != 0) {
                LOG_DEBUG("read_block[", block_id,
                          "]: expected terminator (size=0) chunk_hdr, got size=",
                          last_chunk_hdr->size);
                ec = Errc::expected_no_more_data;
            }
            else {
                LOG_DEBUG("read_block[", block_id,
                          "]: got terminator chunk_hdr — block read complete");
            }
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
    void download_hash_list(
            GenericStream& con,
            std::shared_ptr<unsigned> newest_proto_seen,
            Cancel& timeout_cancel,
            Cancel& cancel,
            asio::yield_context yield)
    {
        sys::error_code ec;

        auto timeout_cancel_con = timeout_cancel.connect([&] { con.close(); });

        http::async_write(con, request(http::verb::propfind, _resource_id), yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        GenericStream stream = determine_incoming_stream(con, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        auto reader = http_response::Reader(std::move(stream));
        //auto reader = std::make_unique<http_response::Reader>(move(con));

        auto hash_list = HashList::load(reader, _cache_pk, timeout_cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);

        if (!util::http_proto_version_check_trusted(hash_list.signed_head, *newest_proto_seen))
          // The client expects an injection belonging to a supported protocol version,
          // otherwise we just discard this copy.
          return or_throw(yield, asio::error::not_found);

        _hash_list = move(hash_list);
        _connection = std::move(con);
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
        , _peer_lookup(move(peer_lookup))
        , _newest_proto_seen(move(newest_proto_seen))
        , _log_path(move(log_path))
        , _random_generator(_random_device())
    {
        if (!_peer_lookup) {
            _cv.notify();
            return;
        }

        if (auto dht_lock = _peer_lookup->get_dht_lock()) {
            for (auto ep : _lan_peer_eps) {
                add_candidate(ep, *dht_lock);
            }
        }

        task::spawn_detached(_exec, [this, log_path = _log_path, c = _lifetime_cancel] (auto y) mutable {
            TRACK_HANDLER();
            sys::error_code ec;

            auto peer_eps = _peer_lookup->get(c, y[ec]);

            LOG_DEBUG(log_path, " Peer lookup result; ec=", ec, " eps=", peer_eps);

            if (c) return;

            if (!ec) {
                if (auto dht_lock = _peer_lookup->get_dht_lock()) {
                    for (auto ep : peer_eps) add_candidate(ep, *dht_lock);
                }
            }

            _peer_lookup.reset();

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

#ifdef __EXPERIMENTAL__
    // Constructor for BEP3 tracker + I2P peers
    // LAN peers some how depends on DHT (lock?) which might have not been initiated
    // so we don't deal with them here.
    Peers(AsioExecutor exec
         , sign::PublicKey cache_pk
         , const ResourceId& resource_id
         , const CryptoStreamKey& resource_key
         , std::shared_ptr<Bep3TrackerLookup> tracker_lookup
         , std::shared_ptr<ouiservice::i2poui::Service> i2p_service
         , std::shared_ptr<unsigned> newest_proto_seen
         , util::LogPath log_path)
        : _exec(exec)
        , _cv(_exec)
        , _cache_pk(move(cache_pk))
        , _resource_id(resource_id)
        , _resource_key(resource_key)
        , _tracker_lookup(move(tracker_lookup))
        , _i2p_service(move(i2p_service))
        , _newest_proto_seen(move(newest_proto_seen))
        , _log_path(move(log_path))
        , _random_generator(_random_device())
    {
        task::spawn_detached(_exec, [this, log_path = _log_path, c = _lifetime_cancel] (auto y) mutable {
            TRACK_HANDLER();
            sys::error_code ec;

            auto i2p_dests = _tracker_lookup->get(c, y[ec]);

            LOG_DEBUG(log_path, " BEP3 tracker lookup result; ec=", ec
                     , " peers=", i2p_dests.size());

            if (c) return;

            if (!ec) {
                for (auto& dest : i2p_dests) add_candidate(dest);
            }

            _tracker_lookup.reset();

            _cv.notify();
        });
    }

    void add_candidate(const string& i2p_dest) {
        auto ip = _all_i2p_peers.insert({i2p_dest, unique_ptr<Peer>()});

        if (!ip.second) return; // Already inserted

        ip.first->second = make_unique<Peer>(_exec, _resource_id, _resource_key, _cache_pk, _log_path);
        Peer* p = ip.first->second.get();

        _candidate_peers.push_back(*p);

        task::spawn_detached(_exec, [=, this, log_path = _log_path,
                                     i2p_service = _i2p_service,
                                     c = _lifetime_cancel] (auto y) mutable {
            TRACK_HANDLER();
            sys::error_code ec;

            // Build+start the i2p_client once — tunnel setup is expensive
            // (5–30s on a real i2p network). 
            LOG_DEBUG(log_path, " building new i2p client to tunnel to dest=", i2p_dest);
            auto i2p_client = i2p_service->build_client(i2p_dest);
            i2p_client->start(y[ec]);
            if (c) return;
            if (ec) {
                LOG_DEBUG(log_path, " BEP3 i2p_client start failed ec=", ec,
                          " — dropping peer: ", i2p_dest);
                p->_candidate_hook.unlink();
                _cv.notify();
                return;
            }
            // Hand ownership to the Peer so
            // the LocalDestination outlives this coroutine; otherwise later
            // writes on `_connection` race with the tunnel teardown and
            // crash.
            p->_i2p_client = std::move(i2p_client);

            // On timeout, retry with exponential-backoff around connect+download_hash_list. 
            // sleep and try again on the SAME client/tunnel — i2pd's
            // LocalDestination already rotates through several inbound/outbound
            // tunnels internally, so a fresh Client::connect() call still gets
            // some path diversity for free.

            // On a non-timeout failure (e.g.
            // Element not found — the peer said it doesn't have the resource),
            // give up on this peer immediately: retrying wouldn't change the
            // answer. The outer _lifetime_cancel (captured as `c`) breaks the
            // loop when the MultiPeerReader shuts down.
            for (uint32_t attempt = 0; ; ++attempt) {
                ec = {};

                Cancel timeout_cancel(c);
                auto wd = watch_dog(_exec, MultiPeerReader::BEP3_HASH_LIST_TIMEOUT, [&] {
                    LOG_DEBUG("I2P BEP3 hash list download timed out for: ", i2p_dest);
                    timeout_cancel();
                });

                LOG_DEBUG(log_path, " connecting to the i2p peer (attempt=",
                          attempt, ")");
                auto con = p->_i2p_client->connect(y[ec], timeout_cancel);
                if (c) return;

                if (!ec) {
                    LOG_DEBUG(log_path, " downloading hash list over i2p...");
                    p->download_hash_list(con, _newest_proto_seen,
                                          timeout_cancel, c, y[ec]);
                    if (c) return;
                }

                LOG_DEBUG(log_path, " Done fetching hash list attempt=", attempt,
                          " i2p_dest=", i2p_dest, " ec=", ec, " c=", bool(c));

                if (!ec) {
                    // Success — promote to good_peers.
                    p->_candidate_hook.unlink();
                    _good_peers.push_back(*p);
                    _cv.notify();
                    return;
                }

                // Distinguish timeout (transient — retry) from a definitive
                // failure (e.g., Element not found — give up on this peer).
                const bool timed_out = !wd.is_running();
                if (!timed_out) {
                    LOG_DEBUG(log_path, " BEP3 peer definitive failure ec=", ec,
                              " — dropping from candidates: ", i2p_dest);
                    p->_candidate_hook.unlink();
                    _cv.notify();
                    return;
                }

                LOG_DEBUG(log_path, " BEP3 timeout on attempt=", attempt,
                          " — backing off before retry: ", i2p_dest);

                ec = {};
                util::exponential_backoff(attempt, c, y[ec]);
                if (ec || c) return;
            }
        });
    }
#endif

    void add_candidate(udp::endpoint ep, const bittorrent::DhtBase& dht) {
        if (dht.is_martian(ep)) return;
        if (_wan_my_eps.count(ep)) return;

        auto ip = _all_udp_peers.insert({ep, unique_ptr<Peer>()});

        auto peer_log_path = _log_path.tag(util::str(ep));

        if (!ip.second) return; // Already inserted

        ip.first->second = make_unique<Peer>(_exec, _resource_id, _resource_key, _cache_pk, peer_log_path);
        Peer* p = ip.first->second.get();

        _candidate_peers.push_back(*p);

        task::spawn_detached(_exec, [=, this, log_path = peer_log_path,
                                     lan_my_eps = _lan_my_eps,
                                     newest_proto_seen = _newest_proto_seen,
                                     c = _lifetime_cancel] (auto y) mutable {
            TRACK_HANDLER();
            sys::error_code ec;

            LOG_DEBUG(log_path, " Fetching hash list from: ", ep);

            Cancel timeout_cancel(c);
            auto wd = watch_dog(_exec, MultiPeerReader::BEP5_HASH_LIST_TIMEOUT, [&] {
                LOG_DEBUG("BEP5 hash list download timed out for: ", ep);
                timeout_cancel();
            });

            auto con = connect(_exec, ep, lan_my_eps, timeout_cancel, y[ec]);
            fail_on_error_or_timeout(y, c, ec, wd);

            p->download_hash_list(con, newest_proto_seen, timeout_cancel, c, y[ec]);

            LOG_DEBUG(log_path, " Done fetching hash list; ep=", ep
                     , " ec=", ec, " c=", bool(c));

            if (c) return;

            p->_candidate_hook.unlink();

            if (!ec) _good_peers.push_back(*p);

            _cv.notify();
        });
    }

    bool still_waiting_for_candidates() const {
        if (_peer_lookup || !_candidate_peers.empty()) return true;
#ifdef __EXPERIMENTAL__
        if (_tracker_lookup) return true;
#endif
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

        LOG_DEBUG("choose_reference_hash_list: chosen peer has blocks.size()=",
                  best_peer->_hash_list.blocks.size());

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
#ifdef __EXPERIMENTAL__
    std::map<string, unique_ptr<Peer>> _all_i2p_peers;
#endif

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
    std::shared_ptr<DhtLookup> _peer_lookup;
#ifdef __EXPERIMENTAL__
    std::shared_ptr<Bep3TrackerLookup> _tracker_lookup;
    std::shared_ptr<ouiservice::i2poui::Service> _i2p_service;
#endif
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
                                , std::set<asio::ip::udp::endpoint> lan_my_eps
                                , std::set<asio::ip::udp::endpoint> wan_my_eps
                                , std::shared_ptr<DhtLookup> peer_lookup
                                , std::shared_ptr<unsigned> newest_proto_seen
                                , util::LogPath log_path)
    : _executor(ex)
    , _log_path(std::move(log_path))
{
    _peers = make_unique<Peers>(ex
                               , move(lan_my_eps)
                               , move(wan_my_eps)
                               , move(lan_peer_eps)
                               , move(cache_pk)
                               , move(resource_id)
                               , move(resource_key)
                               , move(peer_lookup)
                               , move(newest_proto_seen)
                               , _log_path.tag("Peers"));
}

#ifdef __EXPERIMENTAL__
MultiPeerReader::MultiPeerReader( AsioExecutor ex
                                , ResourceId resource_id
                                , CryptoStreamKey resource_key
                                , sign::PublicKey cache_pk
                                , std::shared_ptr<Bep3TrackerLookup> tracker_lookup
                                , std::shared_ptr<ouiservice::i2poui::Service> i2p_service
                                , std::shared_ptr<unsigned> newest_proto_seen
                                , util::LogPath log_path)
    : _executor(ex)
    , _log_path(log_path)
{
    _peers = make_unique<Peers>(ex
                               , move(cache_pk)
                               , move(resource_id)
                               , move(resource_key)
                               , move(tracker_lookup)
                               , move(i2p_service)
                               , move(newest_proto_seen)
                               , log_path);
}
#endif

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
MultiPeerReader::fetch_block(size_t block_id, Cancel &cancel, asio::yield_context yield) {

  // Pipelining: send Q_{n+1} while reading R_n
  //   Q0   Q1   R0   Q2   R1   Q3   R2
  // |----|----|----|----|----|----|----|...
  // somehow doesn't work here. read_block(n) over-reads response[n+1]
  // I think and those bytes are lost. Till this gets fixed we send Q_{n+1} only after
  // read_block(n) has completed.

    using OptBlock = boost::optional<MultiPeerReader::Block>;

    sys::error_code ec;

    if (!_pre_fetch) {
        _pre_fetch = new_fetch_job(block_id, nullptr, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, OptBlock{});

        // new_fetch_job should always return non-null if block_id is valid.
        assert(_pre_fetch);
    }

    auto fetch = std::move(_pre_fetch);

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

