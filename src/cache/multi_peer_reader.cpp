#include <asio_utp.hpp>

#include "multi_peer_reader.h"
#include "multi_peer_reader_error.h"
#include "cache_entry.h"
#include "http_sign.h"
#include "../http_util.h"
#include "../session.h"
#include "../util/watch_dog.h"
#include "../util/crypto.h"
#include "../util/intrusive_list.h"
#include "../bittorrent/is_martian.h"
#include "../constants.h"
#include "../util/set_io.h"
#include "../util/part_io.h"
#include "../util/async_job.h"
#include "../util/condition_variable.h"
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
choose_multiplexer_for( asio::executor exec, const udp::endpoint& ep
                      , const set<udp::endpoint>& local_eps)
{
    for (auto& e : local_eps) {
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
                     , const set<udp::endpoint>& local_eps
                     , Cancel cancel
                     , asio::yield_context yield)
{
    sys::error_code ec;
    auto opt_m = choose_multiplexer_for(exec, ep, local_eps);
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
    util::intrusive::list_hook _good_peer_hook;

    asio::executor _exec;
    string _key;
    const util::Ed25519PublicKey _cache_pk;
    std::unique_ptr<http_response::Reader> _reader;
    HashList _hash_list;
    Cancel _lifetime_cancel;

    Peer(asio::executor exec, const string& key, util::Ed25519PublicKey cache_pk) :
        _exec(exec),
        _key(key),
        _cache_pk(cache_pk)
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

    void send_block_request(size_t block_id, Cancel& c, asio::yield_context yield)
    {
        if (!_reader) return or_throw(yield, asio::error::not_connected);

        sys::error_code ec;

        auto cl = _lifetime_cancel.connect([&] { c(); });
        auto cc = c.connect([&] { if (_reader) _reader->close(); });

        Cancel tc(c);
        auto wd = watch_dog(_exec, WRITE_REQUEST_TIMEOUT, [&] { tc(); });

        http::async_write(_reader->stream(), range_request(http::verb::get, block_id, _key), yield[ec]);

        if (tc) ec = asio::error::timed_out;
        if (c)  ec = asio::error::operation_aborted;

        return or_throw(yield, ec);
    }

    // May return boost::none and no error if the response has no body (e.g. redirect msg)
    boost::optional<Block> read_block(size_t block_id, Cancel& c, asio::yield_context yield)
    {
        using OptBlock = boost::optional<Block>;

        if (!_reader) return or_throw<OptBlock>(yield, asio::error::not_connected);

        sys::error_code ec;

        auto cl = _lifetime_cancel.connect([&] { c(); });
        auto cc = c.connect([&] { if (_reader) _reader->close(); });

        auto head = _reader->timed_async_read_part(READ_HEAD_TIMEOUT, c, yield[ec]);
        if (ec) return or_throw<OptBlock>(yield, ec);

        if (!head || !head->is_head()) {
            return or_throw<OptBlock>(yield, Errc::expected_head);
        }

        auto p = _reader->timed_async_read_part(READ_CHUNK_HDR_TIMEOUT, c, yield[ec]);
        if (ec) return or_throw<OptBlock>(yield, ec);

        // This may happen when the message has no body
        if (!p) {
            _reader->restart();
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
                p = _reader->timed_async_read_part(READ_CHUNK_BODY_TIMEOUT, c, yield[ec]);
                if (ec) return or_throw<OptBlock>(yield, ec);

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

            p = _reader->timed_async_read_part(READ_CHUNK_HDR_TIMEOUT, c, yield[ec]);

            ChunkHdr* last_chunk_hdr = p ? p->as_chunk_hdr() : nullptr;

            if (!last_chunk_hdr) ec = Errc::expected_chunk_hdr;
            else if (last_chunk_hdr->size != 0) ec = Errc::expected_no_more_data;
            if (ec) return or_throw<OptBlock>(yield, ec);
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
            p = _reader->timed_async_read_part(READ_TRAILER_TIMEOUT, c, yield[ec]);
            if (ec) return or_throw<OptBlock>(yield, ec);
            if (!p) {
                // We're done with this request
                _reader->restart();
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

    void download_hash_list(
            udp::endpoint ep,
            const set<udp::endpoint>& local_eps,
            std::shared_ptr<unsigned> newest_proto_seen,
            Cancel cancel,
            asio::yield_context yield)
    {
        sys::error_code ec;

        Cancel timeout_cancel(cancel);

        auto wd = watch_dog(_exec, chrono::seconds(10), [&] { timeout_cancel(); });

        auto con = connect(_exec, ep, local_eps, timeout_cancel, yield[ec]);

        if (timeout_cancel) ec = asio::error::timed_out;
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw(yield, ec);

        auto timeout_cancel_con = timeout_cancel.connect([&] { con.close(); });

        http::async_write(con, request(http::verb::propfind, _key), yield[ec]);

        if (timeout_cancel) ec = asio::error::timed_out;
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw(yield, ec);

        auto reader = std::make_unique<http_response::Reader>(move(con));

        auto hash_list = HashList::load(*reader, _cache_pk, timeout_cancel, yield[ec]);

        if (timeout_cancel) ec = asio::error::timed_out;
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw(yield, ec);

        if (!util::http_proto_version_check_trusted(hash_list.signed_head, *newest_proto_seen))
            // The client expects an injection belonging to a supported protocol version,
            // otherwise we just discard this copy.
            return or_throw(yield, asio::error::not_found);

        _hash_list = move(hash_list);
        reader->restart();
        _reader = std::move(reader);
    }
};


class MultiPeerReader::Peers {
public:
    Peers(asio::executor exec
         , set<udp::endpoint> local_endpoints
         , set<udp::endpoint> wan_endpoints
         , set<udp::endpoint> local_peer_eps
         , util::Ed25519PublicKey cache_pk
         , const std::string& key
         , std::shared_ptr<PeerLookup> peer_lookup
         , std::shared_ptr<unsigned> newest_proto_seen
         , std::string dbg_tag)
        : _exec(exec)
        , _cv(_exec)
        , _cache_pk(move(cache_pk))
        , _local_peer_eps(move(local_peer_eps))
        , _local_endpoints(move(local_endpoints))
        , _our_endpoints(move(wan_endpoints))
        , _key(move(key))
        , _peer_lookup(move(peer_lookup))
        , _newest_proto_seen(move(newest_proto_seen))
        , _dbg_tag(move(dbg_tag))
        , _random_generator(_random_device())
    {
        for (auto ep : _local_peer_eps) {
            add_candidate(ep);
        }

        if (!_peer_lookup) {
            _cv.notify();
            return;
        }

        asio::spawn(_exec, [=, dbg_tag = _dbg_tag, c = _lifetime_cancel] (auto y) mutable {
            TRACK_HANDLER();
            sys::error_code ec;

            auto peer_eps = _peer_lookup->get(c, y[ec]);

            if (!dbg_tag.empty()) {
                LOG_DEBUG(dbg_tag, " Peer lookup result; ec=", ec, " eps=", peer_eps);
            }

            if (c) return;

            _peer_lookup.reset();

            if (!ec) {
                for (auto ep : peer_eps) add_candidate(ep);
            }

            _cv.notify();
        });
    }

    Peers(asio::executor exec
         , set<udp::endpoint> local_endpoints
         , set<udp::endpoint> local_peer_eps
         , util::Ed25519PublicKey cache_pk
         , const std::string& key
         , std::shared_ptr<unsigned> newest_proto_seen
         , std::string dbg_tag)
        : Peers( exec, move(local_endpoints), {}, move(local_peer_eps)
               , move(cache_pk), key, nullptr
               , move(newest_proto_seen), move(dbg_tag))
    {}

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
                LOG_DEBUG(dbg_tag, " Fetching hash list from: ", ep);
            }

            p->download_hash_list(ep, _local_endpoints, _newest_proto_seen, c, y[ec]);

            if (!dbg_tag.empty()) {
                LOG_DEBUG(dbg_tag, " Done fetching hash list; ep=", ep
                         , " ec=", ec, " c=", bool(c));
            }

            if (c) return;

            p->_candidate_hook.unlink();

            if (!ec) _good_peers.push_back(*p);

            _cv.notify();
        });
    }

    bool still_waiting_for_candidates() const {
        return _peer_lookup || !_candidate_peers.empty();
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
        if (ec) return or_throw<HashList>(yield, ec);

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
        if (ec) return or_throw<Peer*>(yield, ec, nullptr);

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
    // Peers that are in _all_peers but are not in either _candidate_peers
    // nor _good_peers are considered as failed.
    std::map<udp::endpoint, unique_ptr<Peer>> _all_peers;

    util::intrusive::list<Peer, &Peer::_candidate_hook> _candidate_peers;
    util::intrusive::list<Peer, &Peer::_good_peer_hook> _good_peers;

    asio::executor _exec;
    ConditionVariable _cv;

    util::Ed25519PublicKey _cache_pk;
    std::set<asio::ip::udp::endpoint> _local_peer_eps;
    std::set<asio::ip::udp::endpoint> _local_endpoints;
    std::set<asio::ip::udp::endpoint> _our_endpoints;
    std::string _key;
    std::shared_ptr<PeerLookup> _peer_lookup;
    std::shared_ptr<unsigned> _newest_proto_seen;
    std::string _dbg_tag;

    Cancel _lifetime_cancel;

    std::random_device _random_device;
    std::mt19937 _random_generator;
};

MultiPeerReader::MultiPeerReader( asio::executor ex
                                , util::Ed25519PublicKey cache_pk
                                , std::set<asio::ip::udp::endpoint> local_peers
                                , std::string key
                                , std::set<asio::ip::udp::endpoint> local_endpoints
                                , std::shared_ptr<unsigned> newest_proto_seen
                                , const std::string& dbg_tag)
    : _executor(ex)
    , _dbg_tag(dbg_tag)
{
    _peers = make_unique<Peers>(ex
                               , move(local_endpoints)
                               , move(local_peers)
                               , move(cache_pk)
                               , move(key)
                               , move(newest_proto_seen)
                               , dbg_tag);
}

MultiPeerReader::MultiPeerReader( asio::executor ex
                                , util::Ed25519PublicKey cache_pk
                                , std::set<asio::ip::udp::endpoint> local_peers
                                , std::string key
                                , const bittorrent::MainlineDht& dht
                                , std::shared_ptr<PeerLookup> peer_lookup
                                , std::shared_ptr<unsigned> newest_proto_seen
                                , const std::string& dbg_tag)
    : _executor(ex)
    , _dbg_tag(dbg_tag)
{
    _peers = make_unique<Peers>(ex
                               , move(dht.local_endpoints())
                               , move(dht.wan_endpoints())
                               , move(local_peers)
                               , move(cache_pk)
                               , move(key)
                               , move(peer_lookup)
                               , move(newest_proto_seen)
                               , dbg_tag);
}

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

struct MultiPeerReader::PreFetchSequential : MultiPeerReader::PreFetch {
    AsyncJob<boost::none_t> job;

    PreFetchSequential(size_t block_id, Peer* peer, asio::executor ex)
        : PreFetch(block_id, peer)
        , job(ex)
    {
        job.start([=] (auto& cancel, auto yield) -> boost::none_t {
            sys::error_code ec;
            peer->send_block_request(block_id, cancel, yield[ec]);
            if (ec) return or_throw(yield, asio::error::operation_aborted, boost::none);
            return boost::none;
        });
    }

    OptBlock get_block(Cancel& cancel, asio::yield_context yield) override {
        sys::error_code ec;

        job.wait_for_finish(cancel, yield[ec]);
        if (ec) return or_throw<OptBlock>(yield, ec);

        return peer->read_block(block_id, cancel, yield);
    }
};

struct MultiPeerReader::PreFetchParallel : MultiPeerReader::PreFetch {
    using Job = AsyncJob<OptBlock>;
    Job job;

    PreFetchParallel(size_t block_id, Peer* peer, asio::executor ex)
        : PreFetch(block_id, peer)
        , job(ex)
    {
        job.start([=] (auto& cancel, auto yield) -> OptBlock {
            sys::error_code ec;
            peer->send_block_request(block_id, cancel, yield[ec]);
            if (ec) return or_throw<OptBlock>(yield, asio::error::operation_aborted);
            return peer->read_block(block_id, cancel, yield);
        });
    }

    OptBlock get_block(Cancel& cancel, asio::yield_context yield) override {
        sys::error_code ec;

        job.wait_for_finish(cancel, yield[ec]);
        if (ec) return or_throw<OptBlock>(yield, ec);

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
    if (ec) return or_throw<R>(yield, ec);

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
        if (ec) return or_throw<OptBlock>(yield, ec);

        // new_fetch_job should always return non-null if block_id is valid.
        assert(_pre_fetch);
    }

    auto fetch = std::move(_pre_fetch);

    _pre_fetch = new_fetch_job(block_id + 1, fetch->peer, cancel, yield[ec]);
    if (ec) return or_throw<OptBlock>(yield, ec);

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
            if (ec) return or_throw<OptBlock>(yield, ec);

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

    if (cancel) return or_throw<Ret>(yield, asio::error::operation_aborted);
    if (_state == State::closed) return or_throw<Ret>(yield, asio::error::bad_descriptor);
    if (_state == State::done) return boost::none;

    auto r = async_read_part_impl(cancel, yield[ec]);

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

    auto lc = _lifetime_cancel.connect([&] { cancel(); });

    if (!_reference_hash_list) {
        auto hl = _peers->choose_reference_hash_list(cancel, yield[ec]);
        if (ec) return or_throw<OptPart>(yield, ec);
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

        if (cancel) ec = asio::error::operation_aborted;
        if (ec) { return or_throw<OptPart>(yield, ec); }

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

