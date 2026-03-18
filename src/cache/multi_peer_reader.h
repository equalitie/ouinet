#pragma once

#include <set>
#include <chrono>
#include <boost/asio/ip/udp.hpp>
#include "../response_reader.h"
#include "../namespaces.h"
#include "dht_lookup.h"
#ifdef __EXPERIMENTAL__
#include "bep3_tracker_lookup.h"
#endif
#include "hash_list.h"
#include "../util/async_generator.h"
#include "../util/log_path.h"
#include "../session.h"
#include "resource_id.h"
#include "util/crypto_stream_key.h"

#ifdef __EXPERIMENTAL__
namespace ouinet::ouiservice::i2poui { class Service; }
#endif

namespace ouinet::cache {

class MultiPeerReader : public http_response::AbstractReader {
private:
    class Peer;
    class Peers;
    struct Block;
    struct PreFetch;
    struct PreFetchSequential;
    struct PreFetchParallel;

    enum class State { active, done, closed };

public:
    // Use this for local cache and LAN retrieval only.
    MultiPeerReader( AsioExecutor ex
                   , ResourceId
                   , CryptoStreamKey
                   , util::Ed25519PublicKey cache_pk
                   , std::set<asio::ip::udp::endpoint> lan_peers
                   , std::set<asio::ip::udp::endpoint> lan_my_endpoints
                   , std::shared_ptr<unsigned> newest_proto_seen
                   , util::LogPath);

    // Use this to include peers on the Internet.
    MultiPeerReader( AsioExecutor ex
                   , ResourceId
                   , CryptoStreamKey
                   , util::Ed25519PublicKey cache_pk
                   , std::set<asio::ip::udp::endpoint> lan_peers
                   , std::set<asio::ip::udp::endpoint> lan_my_endpoints
                   , std::set<asio::ip::udp::endpoint> wan_my_endpoints
                   , std::shared_ptr<DhtLookup> peer_lookup
                   , std::shared_ptr<unsigned> newest_proto_seen
                   , util::LogPath);

#ifdef __EXPERIMENTAL__
    // Use this to include I2P peers via BEP3 tracker.
    MultiPeerReader( AsioExecutor ex
                   , ResourceId
                   , CryptoStreamKey
                   , util::Ed25519PublicKey cache_pk
                   , std::shared_ptr<Bep3TrackerLookup> tracker_lookup
                   , std::shared_ptr<ouiservice::i2poui::Service> i2p_service
                   , std::shared_ptr<unsigned> newest_proto_seen
                   , util::LogPath);
#endif

    MultiPeerReader(MultiPeerReader&&) = delete;
    MultiPeerReader(const MultiPeerReader&) = delete;

    boost::optional<http_response::Part> async_read_part(Cancel, asio::yield_context) override;

    bool is_done() const override
    {
        return _state == State::done;
    }

    void close() override;

    ~MultiPeerReader();

    AsioExecutor get_executor() override
    {
        return _executor;
    }

private:
    boost::optional<http_response::Part> async_read_part_impl(Cancel&, asio::yield_context);
    boost::optional<Block> fetch_block(size_t block_id, Cancel&, asio::yield_context);
    void unmark_as_good(Peer& peer);

    void mark_done();

    std::unique_ptr<PreFetch>
    new_fetch_job(size_t block_id, Peer* last_peer, Cancel&, asio::yield_context);

    static constexpr std::chrono::seconds BEP5_HASH_LIST_TIMEOUT{10};
#ifdef __EXPERIMENTAL__
    static constexpr std::chrono::seconds BEP3_HASH_LIST_TIMEOUT{30};
#endif

private:
    AsioExecutor _executor;
    Cancel _lifetime_cancel;

    boost::optional<HashList> _reference_hash_list;
    std::unique_ptr<Peers> _peers;
    util::LogPath _log_path;
    bool _head_sent = false;
    size_t _block_id = 0;

    std::string _next_chunk_hdr_ext;
    boost::optional<http_response::ChunkBody> _next_chunk_body;
    boost::optional<http_response::Trailer> _next_trailer;
    bool _last_chunk_hdr_sent = false;

    State _state = State::active;

    std::unique_ptr<PreFetch> _pre_fetch;
};

} // namespaces
