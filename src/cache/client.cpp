#include "client.h"
#include "announcer.h"
#include "dht_lookup.h"
#include "local_peer_discovery.h"
#include "http_sign.h"
#include "http_store.h"
#include "resource_key.h"
#include "../default_timeout.h"
#include "../http_util.h"
#include "../parse/number.h"
#include "../util/set_io.h"
#include "../util/lru_cache.h"
#include "../task.h"
#include "../util/keep_alive.h"
#include "../util/crypto_stream.h"
#include "../ouiservice/utp.h"
#include "request.h"
#include "ouiservice/i2p/tracker.h"
#include "ouiservice/i2p/tracker_lookup.h"
#include "ouiservice/i2p/announcer.h"
#include "../logger.h"
#include "../async_sleep.h"
#include "../constants.h"
#include "../session.h"
#include "../bep5_swarms.h"
#include "multi_peer_reader.h"
#include <map>
#include <string>

#define _LOGPFX "cache/client: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _VERBOSE(...) LOG_VERBOSE(_LOGPFX, __VA_ARGS__)
#define _INFO(...)  LOG_INFO(_LOGPFX, __VA_ARGS__)
#define _WARN(...)  LOG_WARN(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)
#define _YDEBUG(y, ...) do { if (get_logger().get_threshold() <= DEBUG) LOG_DEBUG(y, " ", __VA_ARGS__); } while (false)
#define _YERROR(y, ...) do { if (get_logger().get_threshold() <= ERROR) LOG_ERROR(y, " ", __VA_ARGS__); } while (false)

using namespace std;
using namespace ouinet;
using namespace ouinet::cache;
using udp = asio::ip::udp;
using cache::ResourceId;

namespace fs = boost::filesystem;
namespace bt = bittorrent;

struct GarbageCollector {
    cache::HttpStore& http_store;  // for looping over entries
    cache::HttpStore::keep_func keep;  // caller-provided checks

    util::LogPath _log_path;
    AsioExecutor _executor;
    Cancel _cancel;

    GarbageCollector( cache::HttpStore& http_store
                    , cache::HttpStore::keep_func keep
                    , util::LogPath log_path
                    , AsioExecutor ex)
        : http_store(http_store)
        , keep(std::move(keep))
        , _log_path(std::move(log_path))
        , _executor(ex)
    {}

    ~GarbageCollector() { _cancel(); }

    void start()
    {
        spawn_detached(_executor, _cancel, _log_path, [&] (Async yield) {
            LOG_DEBUG(yield, " Garbage collector started");
            while (true) {
                async_sleep(chrono::minutes(7), yield);

                LOG_DEBUG(yield, " Collecting garbage...");
                auto r = http_store.for_each([&] (cache::ResourceId const& resource_id, auto rr, Async y) {
                    return keep(resource_id, std::move(rr), y);
                }, yield);
                if (!r) LOG_WARN(yield, " Collecting garbage: failed; ec=", r.error());
                else LOG_DEBUG(yield, " Collecting garbage: done");
            }
        });
    }
};

struct Client::Impl {
    using GroupName = Client::GroupName;
    using BaseGroups = BaseDhtGroups;

    // The newest protocol version number seen in a trusted exchange
    // (i.e. from injector-signed cached content).
    std::shared_ptr<unsigned> _newest_proto_seen;

    AsioExecutor _ex;
    std::set<udp::endpoint> _lan_my_endpoints;
    shared_ptr<bt::DhtBase> _dht;
    string _uri_swarm_prefix;
    sign::PublicKey _cache_pk;
    fs::path _cache_dir;
    Client::opt_path _static_cache_dir;
    unique_ptr<cache::HttpStore> _http_store;
    boost::posix_time::time_duration _max_cached_age;
    Cancel _lifetime_cancel;
    std::unique_ptr<Bep5Announcer> _bep5_announcer;
    std::shared_ptr<I2pTrackerClient> _i2p_tracker;
    std::unique_ptr<I2pAnnouncer> _i2p_announcer;
    GarbageCollector _gc;
    map<string, udp::endpoint> _peer_cache;
    util::LruCache<std::string, shared_ptr<DhtLookup>> _dht_peer_lookups;
    util::LruCache<std::string, shared_ptr<I2pTrackerLookup>> _i2p_peer_lookups;
    LocalPeerDiscovery _local_peer_discovery;
    std::unique_ptr<DhtGroups> _groups;
    util::LogPath _log_path;

    Impl( AsioExecutor ex
        , std::set<udp::endpoint> lan_my_eps
        , sign::PublicKey& cache_pk
        , fs::path cache_dir
        , Client::opt_path static_cache_dir
        , unique_ptr<cache::HttpStore> http_store_
        , boost::posix_time::time_duration max_cached_age
        , util::LogPath log_path)
        : _newest_proto_seen(std::make_shared<unsigned>(http_::protocol_version_current))
        , _ex(ex)
        , _lan_my_endpoints(std::move(lan_my_eps))
        , _uri_swarm_prefix(bep5::compute_uri_swarm_prefix
              (cache_pk, http_::protocol_version_current))
        , _cache_pk(cache_pk)
        , _cache_dir(std::move(cache_dir))
        , _static_cache_dir(std::move(static_cache_dir))
        , _http_store(std::move(http_store_))
        , _max_cached_age(max_cached_age)
        , _gc(*_http_store, [&] (const auto& resource_id, auto rr, auto y) {
              return keep_cache_entry(resource_id, std::move(rr), y);
          }, log_path, _ex)
        , _dht_peer_lookups(256)
        , _i2p_peer_lookups(256)
        , _local_peer_discovery(_ex, _lan_my_endpoints)
        , _log_path(std::move(log_path))
    {}

    std::string compute_swarm_name(boost::string_view group) const {
        return bep5::compute_uri_swarm_name(
                _uri_swarm_prefix,
                group);
    }

    bool enable_i2p(std::shared_ptr<I2pSession> i2p_session, I2pAddress tracker_addr) {
        if (_i2p_announcer) {
            _DEBUG("BEP3 announcer is already enabled");
            return false;
        }

        _i2p_tracker = std::make_shared<I2pTrackerClient>(i2p_session, tracker_addr);
        _i2p_announcer = std::make_unique<I2pAnnouncer>(_i2p_tracker);

        // Announce all groups.
        for (auto& group_name : _groups->groups())
            if (_i2p_announcer->add(util::sha1_digest(compute_swarm_name(group_name))))
                _VERBOSE("Start I2P announcing group: ", group_name);

        _DEBUG("I2P announcer successfully initiated");
        return true;
    }

    bool enable_dht(shared_ptr<bt::DhtBase> dht, size_t simultaneous_announcements) {
        if (_dht || _bep5_announcer) return false;

        _dht = std::move(dht);
        _bep5_announcer = std::make_unique<Bep5Announcer>(
            _dht,
            simultaneous_announcements,
            _log_path.tag("announcer")
        );

        // Announce all groups.
        for (auto& group_name : _groups->groups())
            if (_bep5_announcer->add(compute_swarm_name(group_name)))
                _VERBOSE("Start announcing group: ", group_name);

        return true;
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    serve_local( const PeerCacheRequest& req
               , GenericStream& sink
               , metrics::Client& metrics_client
               , Async yield)
    {
        sys::error_code ec;

        _YDEBUG(yield, "Start\n", req);

        auto make_crypto_sink = [&sink] (CryptoStreamKey const& key) -> GenericStream {
            return CryptoStream<StreamRef<GenericStream>>(sink, key);
        };

        // Usually we would
        // (1) check that the request matches our protocol version, and
        // (2) check that we can derive a resource_id to look up the local cache.
        // However, we still want to blindly send a response we have cached
        // if the request looks like a Ouinet one and we can derive a resource_id,
        // to help the requesting client get the result and other information
        // like a potential new protocol version.
        // The requesting client may choose to drop the response
        // or attempt to extract useful information from it.

        _YDEBUG(yield, "Received request for ", req.resource_id());

        if (req.method() == http::verb::propfind) {
            _YDEBUG(yield, "Serving propfind for ", req.resource_id());
            auto hl = _http_store->load_hash_list(req.resource_id(), yield);

            CryptoStreamKey key;

            if (hl) {
                auto opt_key = resource_key::from_cached_header(hl->signed_head);
                if (!opt_key) {
                    hl = std::unexpected(asio::error::not_found);
                } else {
                    key = *opt_key;
                }
            }

            if (!hl) {
                return handle_not_found(sink, req.keep_alive(), yield);
            }

            if (auto r = async_write_blob_type(BlobType::cypher_text, sink, yield); !r) {
                return std::unexpected(r.error());
            }

            auto crypto_sink = make_crypto_sink(key);
            if (auto r = hl->write(crypto_sink, yield.tag("write_propfind")); !r) {
                return std::unexpected(r.error());
            }

            return {};
        }

        cache::reader_uptr rr;

        if (auto range = req.range()) {
            if (auto r = _http_store->range_reader(req.resource_id(), range->first, range->last, yield)) {
                rr = std::move(*r);
            }
            else {
                _YDEBUG(yield, "Not serving: ", req.resource_id(), "; ec=", r.error());
                return handle_not_found(sink, req.keep_alive(), yield);
            }
        } else {
            if (auto r = _http_store->reader(req.resource_id(), yield)) {
                rr = std::move(*r);
            }
            else {
                _YDEBUG(yield, "Not serving: ", req.resource_id(), "; ec=", r.error());
                return handle_not_found(sink, req.keep_alive(), yield);
            }
        }

        _YDEBUG(yield, "BEGIN");

        // Remember to always set `ec` before return in case of error,
        // or the wrong error code will be reported.
        size_t fwd_bytes = 0;
        auto log_result = defer([&] {
            _YDEBUG(yield, "END; ec=", ec, " fwd_bytes=", fwd_bytes);
        });

        _YDEBUG(yield, "Serving: ", req.resource_id());

        auto s = Session::create(
                std::move(rr),
                req.method() == http::verb::head,
                metrics_client.new_cache_out_request(),
                yield.tag("read_hdr")
            );

        CryptoStreamKey key;

        if (s) {
            auto opt_key = resource_key::from_cached_header(s->response_header());
            if (!opt_key) {
                s = std::unexpected(asio::error::not_found);
            } else {
                key = *opt_key;
            }
        }

        if (!s) return std::unexpected(s.error());

        //bool keep_alive = req.keep_alive() && s->response_header().keep_alive();

        if (auto r = async_write_blob_type(BlobType::cypher_text, sink, yield); !r) {
            return std::unexpected(r.error());
        }

        auto crypto_sink = make_crypto_sink(key);

        auto r = s->flush_response(yield.tag("flush"),
                    [&crypto_sink, &fwd_bytes] (auto&& part, auto yy) -> std::expected<void, sys::error_code> {
                auto r = part.async_write(crypto_sink, yy);
                if (!r) return std::unexpected(r.error());
                if (auto b = part.as_body())
                    fwd_bytes += b->size();
                else if (auto cb = part.as_chunk_body())
                    fwd_bytes += cb->size();
                return {};
            }, default_timeout::activity());

        if (!r) return std::unexpected(r.error());
        return {};
    }

    [[nodiscard]]
    std::expected<std::size_t, sys::error_code>
    local_size(Async yield) const
    {
        return _http_store->size(yield);
    }

    [[nodiscard]]
    std::expected<void, sys::error_code> local_purge(Async yield)
    {
        // TODO: avoid overlapping with garbage collector
        _DEBUG("Purging local cache...");

        auto r = _http_store->for_each([&] (auto& resource_id, auto rr, auto y) {
            // TODO: Implement specific purge operations
            // for DHT groups and announcer
            // to avoid having to parse all stored heads.
            auto hdr = read_response_header(*rr, y);
            if (!hdr) return false;

            /*
             * `group_pinned` is passed by reference to `unpublished_cache_entry`
             * and then is passed again to `ouinet::DhtGroups::remove` just to
             * avoid traversing the whole file structure twice when checking if
             * a group is pinned or not.
             */
            bool group_pinned = false;
            unpublish_cache_entry(resource_id, group_pinned);
            if (group_pinned) return true; // keep entries of pinned groups

            return false;  // remove entries that are not pinned
        }, yield);

        if (!r) {
            _ERROR("Purging local cache: failed; ec=", r.error());
            return std::unexpected(r.error());
        }

        _DEBUG("Purging local cache: done");
        return {};
    }

    bool pin_group(const GroupName& group_name, sys::error_code& ec)
    {
        return _groups->pin_group(group_name, ec);
    }

    bool unpin_group(const GroupName& group_name, sys::error_code& ec)
    {
        return _groups->unpin_group(group_name, ec);
    }

    bool is_pinned_group(const GroupName& group_name, sys::error_code& ec)
    {
        return _groups->is_pinned(group_name, ec);
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    handle_http_error( GenericStream& con
                     , bool keep_alive
                     , http::status status
                     , const string& proto_error
                     , Async yield)
    {
        if (auto r = async_write_blob_type(BlobType::plain_text, con, yield); !r) {
            return std::unexpected(r.error());
        }
        auto res = util::http_error(keep_alive, status, OUINET_CLIENT_SERVER_STRING, proto_error);
        if (sys::error_code ec = util::http_reply(con, res, yield)) {
            return std::unexpected(ec);
        }
        return {};
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    handle_not_found(GenericStream& con, bool keep_alive, Async yield)
    {
        return handle_http_error( con, keep_alive, http::status::not_found
                                , http_::response_error_hdr_retrieval_failed, yield);
    }

    shared_ptr<DhtLookup> dht_peer_lookup(std::string swarm_name)
    {
        assert(_dht);

        auto* lookup = _dht_peer_lookups.get(swarm_name);

        if (!lookup) {
            lookup = _dht_peer_lookups.put( swarm_name
                                      , make_shared<DhtLookup>(_dht, swarm_name));
        }

        return *lookup;
    }

    shared_ptr<I2pTrackerLookup> i2p_peer_lookup(std::string swarm_name)
    {
        assert(_i2p_tracker);

        auto* lookup = _i2p_peer_lookups.get(swarm_name);

        if (!lookup) {
            lookup = _i2p_peer_lookups.put( swarm_name
                , make_shared<I2pTrackerLookup>(
                    _i2p_tracker, util::sha1_digest(swarm_name)));
        }

        return *lookup;
    }

    [[nodiscard]]
    std::expected<Session, sys::error_code>
    load( const CachePeerRetrieveRequest& request
        , metrics::Client& metrics_client
        , Async yield)
    {
        auto& resource_id = request.resource_id();
        auto& resource_key = request.resource_key();
        auto& group = request.dht_group();
        bool is_head_request = request.method() == http::verb::head;

        LOG_DEBUG(yield, " Requesting from the cache: ", resource_id);

        std::size_t rs_sz = 0;
        auto rs = load_from_local(resource_id, is_head_request, rs_sz, yield);
        LOG_DEBUG(yield, " Looking up local cache; ec=", rs ? sys::error_code() : rs.error());

        if (rs) {
            // TODO: Check its age, store it if it's too old but keep trying
            // other peers.
            if (is_head_request) {
                return std::move(*rs);  // do not care about body size
            }

            auto data_size_sv = rs->response_header()[http_::response_data_size_hdr];
            auto data_size_o = parse::number<std::size_t>(data_size_sv);
            if (data_size_o && rs_sz == *data_size_o) {
                return std::move(*rs);  // local copy available and complete, use it
            }
            // TODO: Ideally, an incomplete or stale local cache entry
            // could be reused in the multi-peer download below.
        }

        util::LogPath log_path = yield.log_path().tag("multi_peer_reader");

        LOG_DEBUG(yield, " Distributed cache lookup: ", request.cache_type());
        LOG_DEBUG(yield, "    dht=", (_dht ? "yes" : "no"));
        LOG_DEBUG(yield, "    i2p=", (_i2p_tracker ? "yes" : "no"));

        using VisitR = std::expected<std::unique_ptr<MultiPeerReader>, sys::error_code>;

        auto reader = request.cache_type().visit(overloaded {
                [&] (CacheType::Bep5Http) -> VisitR {
                    auto peer_lookup_ = dht_peer_lookup(compute_swarm_name(group));

                    auto local_peers = _local_peer_discovery.found_peers();

                    if (_dht) {
                        if (get_logger().get_threshold() <= DEBUG) {
                            LOG_DEBUG(yield, " Peer lookup with DHT and local discovery:");
                            LOG_DEBUG(yield, "    resource_id= ", resource_id);
                            LOG_DEBUG(yield, "    group=       ", group);
                            LOG_DEBUG(yield, "    swarm_name=  ", peer_lookup_->swarm_name());
                            LOG_DEBUG(yield, "    infohash=    ", peer_lookup_->infohash());
                            LOG_DEBUG(yield, "    local_peers= ", local_peers);
                        };

                        return std::make_unique<MultiPeerReader>
                            ( _ex
                            , resource_id
                            , resource_key
                            , _cache_pk
                            , std::move(local_peers)
                            , std::move(peer_lookup_)
                            , _newest_proto_seen
                            , log_path);
                    }
                    else {
                        if (get_logger().get_threshold() <= DEBUG) {
                            LOG_DEBUG(yield, " Peer lookup with local discovery only:");
                            LOG_DEBUG(yield, "    resource_id= ", resource_id);
                            LOG_DEBUG(yield, "    local_peers= ", local_peers);
                        };

                        return std::make_unique<MultiPeerReader>
                            ( _ex
                            , resource_id
                            , resource_key
                            , _cache_pk
                            , std::move(local_peers)
                            , _lan_my_endpoints
                            , _newest_proto_seen
                            , log_path);
                    }
                },
                [&] (CacheType::Bep3HTTPOverI2P) -> VisitR {
                    if (!_i2p_tracker) {
                        return std::unexpected(asio::error::no_protocol_option);
                    }

                    auto i2p_lookup = i2p_peer_lookup(compute_swarm_name(group));

                    auto local_peers = _local_peer_discovery.found_peers();

                    if (get_logger().get_threshold() <= DEBUG) {
                        LOG_DEBUG(yield, " Peer lookup with I2P tracker and local discovery:");
                        LOG_DEBUG(yield, "    resource_id= ", resource_id);
                        LOG_DEBUG(yield, "    group=       ", group);
                        LOG_DEBUG(yield, "    infohash=    ", i2p_lookup->infohash());
                        LOG_DEBUG(yield, "    local_peers= ", local_peers);
                    };

                    // Using I2P specific constructor which doesn't deal with lan endpoints
                    // for now to make it less complicated.
                    return std::make_unique<MultiPeerReader>
                        ( _ex
                        , resource_id
                        , resource_key
                        , _cache_pk
                        , std::move(i2p_lookup)
                        , _i2p_tracker->get_session()
                        , _newest_proto_seen
                        , log_path);
                }
            });

        if (!reader) return std::unexpected(reader.error());

        auto s =  Session::create(
                std::move(*reader),
                is_head_request,
                metrics_client.new_cache_in_request(),
                yield.tag("read_hdr"));

        if (s) {
            s->response_header().set( http_::response_source_hdr  // for agent
                                    , http_::response_source_hdr_dist_cache);
            return std::move(*s);
        }
        else if (rs) {
            _YDEBUG(yield, "Multi-peer session creation failed, falling back to incomplete local copy;"
                    " ec=", s.error());
            // Do not use `.set` as several warnings may co-exist
            // (RFC7234#5.5).
            rs->response_header().insert( http::field::warning
                                        , "119 Ouinet \"Using incomplete response body from local cache\"");
            return std::move(*rs);
        }
        else {
            return std::unexpected(s.error());
        }
    }

    [[nodiscard]]
    std::expected<Session, sys::error_code>
    load_from_local( const ResourceId& resource_id
                   , bool is_head_request
                   , std::size_t& body_size
                   , Async yield)
    {
        cache::reader_uptr rr;

        if (is_head_request) {
            if (auto r = _http_store->reader(resource_id, yield)) {
                std::tie(rr, body_size) = std::pair(std::move(*r), 0);
            }
            else {
                return std::unexpected(r.error());
            }
        }
        else {
            if (auto r = _http_store->reader_and_size(resource_id, yield)) {
                std::tie(rr, body_size) = std::move(*r);
            }
            else {
                return std::unexpected(r.error());
            }
        }

        auto rs = Session::create(std::move(rr), is_head_request, yield.tag("read_hdr"));
        if (!rs) return std::unexpected(rs.error());

        rs->response_header().set( http_::response_source_hdr  // for agent
                                 , http_::response_source_hdr_local_cache);
        return std::move(*rs);
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    store( const ResourceId& resource_id
         , const GroupName& group
         , http_response::AbstractReader& r
         , Async yield)
    {
        cache::KeepSignedReader fr(r);
        if (auto r = _http_store->store(resource_id, fr, yield); !r) {
            return std::unexpected(r.error());
        }

        if (auto r = _groups->add(group, resource_id, yield); !r) {
            return std::unexpected(r.error());
        }

        if (_bep5_announcer) {
            if (_bep5_announcer->add(compute_swarm_name(group)))
                _VERBOSE("Start announcing group: ", group);
        }

        if (_i2p_announcer) {
            if (_i2p_announcer->add(util::sha1_digest(compute_swarm_name(group))))
                _VERBOSE("Start BEP3 announcing group: ", group);
        }

        return {};
    }

    [[nodiscard]]
    std::expected<http::response_header<>, sys::error_code>
    read_response_header(http_response::AbstractReader& reader, Async yield)
    {
        auto slot = _lifetime_cancel.connect([&] { yield.cancel(); });

        auto part = reader.async_read_part(yield);

        if (!part) {
            return std::unexpected(part.error());
        }

        if (!*part) {
            return std::unexpected(sys::errc::make_error_code(sys::errc::no_message));
        }

        auto head = (*part)->as_head(); assert(head);
        return *head;
    }

    // Return maximum if not available.
    boost::posix_time::time_duration
    cache_entry_age(const http::response_header<>& head)
    {
        using ssecs = std::chrono::seconds;
        using bsecs = boost::posix_time::seconds;

        static auto max_age = bsecs(ssecs::max().count());

        auto ts_sv = util::http_injection_ts(head);
        if (ts_sv.empty()) return max_age;  // missing header or field
        auto ts_o = parse::number<ssecs::rep>(ts_sv);
        if (!ts_o) return max_age;  // malformed creation time stamp
        auto now = ssecs(std::time(nullptr));  // as done by injector
        auto age = now - ssecs(*ts_o);
        return bsecs(age.count());
    }

    inline
    void unpublish_cache_entry(const cache::ResourceId& resource_id)
    {
        bool _ = false; // pinned group checks are not needed here
        unpublish_cache_entry(resource_id, _);
    }

    inline
    void unpublish_cache_entry(const cache::ResourceId& resource_id, bool& group_pinned)
    {
        auto empty_groups = _groups->remove(resource_id, group_pinned);
        if (group_pinned) return;

        if (!_bep5_announcer && !_i2p_announcer) return;

        for (const auto& eg : empty_groups) {
            if (_bep5_announcer && _bep5_announcer->remove(compute_swarm_name(eg)))
                _VERBOSE("Stop announcing group: ", eg);

            if (_i2p_announcer && _i2p_announcer->remove(util::sha1_digest(compute_swarm_name(eg))))
                _VERBOSE("Stop BEP3 announcing group: ", eg);
        }
    }

    // Return whether the entry should be kept in storage.
    [[nodiscard]]
    std::expected<bool, sys::error_code>
    keep_cache_entry(const cache::ResourceId& resource_id, cache::reader_uptr rr, Async yield)
    {
        // This should be available to
        // allow removing resource_ids of entries to be evicted.
        assert(_groups);

        auto hdr = read_response_header(*rr, yield);
        if (!hdr) return std::unexpected(hdr.error());

        if ((*hdr)[http_::protocol_version_hdr] != http_::protocol_version_hdr_current) {
            _WARN( "Cached response contains an invalid "
                 , http_::protocol_version_hdr
                 , " header field; removing");
            return false;
        }

        auto age = cache_entry_age(*hdr);
        if (age > _max_cached_age) {
            _DEBUG( "Cached response is too old; removing: "
                  , age, " > ", _max_cached_age
                  , "; resource_id=", resource_id );

            if (_groups->is_pinned(resource_id)) {
                _DEBUG("Keep ", resource_id, " even if it is old because it is pinned");
                return true;
            }

            unpublish_cache_entry(resource_id);
            return false;
        }

        return true;
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    load_stored_groups(Async yield)
    {
        static const auto groups_curver_subdir = "dht_groups";

        auto slot = _lifetime_cancel.connect([&] { yield.cancel(); });

        // Use static groups if its directory is provided.
        std::unique_ptr<BaseGroups> static_groups;
        if (_static_cache_dir) {
            auto groups_dir = *_static_cache_dir / groups_curver_subdir;
            if (!is_directory(groups_dir)) {
                _ERROR("No groups of supported version under static cache, ignoring: ", *_static_cache_dir);
            } else {
                if (auto r = load_static_dht_groups(std::move(groups_dir), yield)) {
                    static_groups = std::move(*r);
                }
                else {
                    _ERROR("Failed to load static groups, ignoring: ", *_static_cache_dir);
                }
            }
        }

        auto groups_dir = _cache_dir / groups_curver_subdir;

        if (auto r = static_groups
                ? load_backed_dht_groups(groups_dir, std::move(static_groups), yield)
                : load_dht_groups(groups_dir, yield)) {
            _groups = std::move(*r);
        }
        else {
            return std::unexpected(r.error());
        }

        auto r = _http_store->for_each([&] (const auto& resource_id, auto rr, Async yield) {
            return keep_cache_entry(resource_id, std::move(rr), yield);
        }, yield);
        if (!r) return std::unexpected(r.error());

        // These checks are not bullet-proof, but they should catch some inconsistencies
        // between resource groups and the HTTP store.
        std::set<ResourceId> bad_items;
        std::set<BaseGroups::GroupName> bad_groups;
        for (auto& group_name : _groups->groups()) {
            unsigned good_items = 0;
            for (auto& group_item : _groups->items(group_name)) {
                // TODO: This implies opening all cache items (again for local cache), make lighter.
                if (auto r = _http_store->reader(group_item, yield); r && *r)
                    good_items++;
                else {
                    _WARN("Group resource missing from HTTP store: ", group_item, " (", group_name, ")");
                    bad_items.insert(group_item);
                }
            }
            if (good_items == 0) {
                _WARN("Not announcing group with no resources in HTTP store: ", group_name);
                bad_groups.insert(group_name);
            }
        }

        for (auto& group_name : bad_groups)
            _groups->remove_group(group_name);

        for (auto& item_name : bad_items)
            _groups->remove(item_name);

        return {};
    }

    void stop() {
        _lifetime_cancel();
        _local_peer_discovery.stop();
    }

    unsigned get_newest_proto_version() const {
        return *_newest_proto_seen;
    }

    std::set<GroupName> get_groups() const {
        return _groups->groups();
    }

    std::set<GroupName> get_pinned_groups() const {
        return _groups->pinned_groups();
    }
};

/* static */
std::expected<std::shared_ptr<Client>, sys::error_code>
Client::build( std::set<udp::endpoint> lan_my_eps
             , sign::PublicKey cache_pk
             , fs::path cache_dir
             , boost::posix_time::time_duration max_cached_age
             , Client::opt_path static_cache_dir
             , Client::opt_path static_cache_content_dir
             , Async yield)
{
    auto ex = yield.get_executor();

    static const auto store_oldver_subdirs = {"data", "data-v1", "data-v2", "data-v3"};
    static const auto store_curver_subdir = cache::root_fname;

    sys::error_code ec;

    // Use a static HTTP store if its directories are provided.
    std::unique_ptr<BaseHttpStore> static_http_store;
    if (static_cache_dir) {
        assert(static_cache_content_dir);
        auto store_dir = *static_cache_dir / store_curver_subdir;
        fs::path canon_content_dir;
        if (!is_directory(store_dir)) {
            ec = asio::error::invalid_argument;
            _ERROR("No HTTP store of supported version under static cache, ignoring: ", *static_cache_dir);
        } else {
            canon_content_dir = fs::canonical(*static_cache_content_dir, ec);
            if (ec) _ERROR( "Failed to make static cache content directory canonical, ignoring: "
                          , *static_cache_content_dir);
        }
        if (!ec)
            // This static store should verify everything loaded from storage
            // (as its source may not be trustworthy),
            // which is not strictly needed for serving content to other clients
            // as they should verify on their own.
            // Nonetheless it may still help identify invalid or malicious content in it
            // before further propagating it.
            // The verification is also done for content retrieved for the local agent,
            // and in this case it is indeed desirable to do so.
            static_http_store = make_static_http_store( std::move(store_dir)
                                                      , std::move(canon_content_dir)
                                                      , cache_pk
                                                      , ex);
        ec = {};
    }

    // Remove obsolete stores.
    for (const auto& dirn : store_oldver_subdirs) {
        auto old_store_dir = cache_dir / dirn;
        if (!is_directory(old_store_dir)) continue;
        _INFO("Removing obsolete HTTP store...");
        fs::remove_all(old_store_dir, ec);
        if (ec) _ERROR("Removing obsolete HTTP store: failed; ec=", ec);
        else _INFO("Removing obsolete HTTP store: done");
        ec = {};
    }

    auto store_dir = cache_dir / store_curver_subdir;
    fs::create_directories(store_dir, ec);
    if (ec) return std::unexpected(ec);

    auto http_store = static_http_store
        ? make_backed_http_store(std::move(store_dir), std::move(static_http_store), ex)
        : make_http_store(std::move(store_dir), ex);

    unique_ptr<Impl> impl(new Impl( ex, std::move(lan_my_eps)
                                  , cache_pk, std::move(cache_dir), std::move(static_cache_dir)
                                  , std::move(http_store), max_cached_age, yield.log_path()));

    if (auto r = impl->load_stored_groups(yield); !r) {
        return std::unexpected(r.error());
    }
    impl->_gc.start();

    return shared_ptr<Client>(new Client(std::move(impl)));
}

Client::Client(unique_ptr<Impl> impl)
    : _impl(std::move(impl))
{}

bool Client::enable_dht(shared_ptr<bt::DhtBase> dht, size_t simultaneous_announcements) {
    return _impl->enable_dht(std::move(dht),
                             simultaneous_announcements);
}

bool Client::enable_i2p( std::shared_ptr<I2pSession> i2p_session
                       , I2pAddress tracker_id) {
    return _impl->enable_i2p(i2p_session, std::move(tracker_id));
}

std::expected<Session, sys::error_code>
Client::load( const CachePeerRetrieveRequest& request
            , metrics::Client& metrics
            , Async yield)
{
    return _impl->load(request, metrics, yield);
}

std::expected<void, sys::error_code>
Client::store( const cache::ResourceId& key
             , const GroupName& group
             , http_response::AbstractReader& r
             , Async yield)
{
    return _impl->store(key, group, r, yield);
}

std::expected<void, sys::error_code>
Client::serve_local( const PeerCacheRequest& req
                   , GenericStream& sink
                   , metrics::Client& metrics
                   , Async yield)
{
    return _impl->serve_local(req, sink, metrics, yield);
}

std::expected<std::size_t, sys::error_code>
Client::local_size(Async yield) const
{
    return _impl->local_size(yield);
}

std::expected<void, sys::error_code>
Client::local_purge(Async yield)
{
    return _impl->local_purge(yield);
}

bool Client::pin_group(const GroupName& group_name, sys::error_code& ec)
{
    return _impl->pin_group(group_name, ec);
}

bool Client::unpin_group(const GroupName& group_name, sys::error_code& ec)
{
    return _impl->unpin_group(group_name, ec);
}

bool Client::is_pinned_group(const GroupName& group_name, sys::error_code& ec)
{
    return _impl->is_pinned_group(group_name, ec);
}

unsigned Client::get_newest_proto_version() const
{
    return _impl->get_newest_proto_version();
}

std::set<Client::GroupName> Client::get_groups() const
{
    return _impl->get_groups();
}

std::set<Client::GroupName> Client::get_pinned_groups()
{
    return _impl->get_pinned_groups();
}

Client::~Client()
{
    _impl->stop();
}
