#include "client.h"
#include "announcer.h"
#include "dht_lookup.h"
#include "local_peer_discovery.h"
#include "dht_groups.h"
#include "http_sign.h"
#include "http_store.h"
#include "../http_util.h"
#include "../parse/number.h"
#include "../util/set_io.h"
#include "../util/lru_cache.h"
#include "../bittorrent/dht.h"
#include "../ouiservice/utp.h"
#include "../logger.h"
#include "../constants.h"
#include "../session.h"
#include "../bep5_swarms.h"
#include "multi_peer_reader.h"
#include <map>

#define _LOGPFX "cache/client: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _INFO(...)  LOG_INFO(_LOGPFX, __VA_ARGS__)
#define _WARN(...)  LOG_WARN(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)
#define _YDEBUG(y, ...) do { if (logger.get_threshold() <= DEBUG) y.log(DEBUG, __VA_ARGS__); } while (false)
#define _YERROR(y, ...) do { if (logger.get_threshold() <= ERROR) y.log(ERROR, __VA_ARGS__); } while (false)

using namespace std;
using namespace ouinet;
using namespace ouinet::cache;
using udp = asio::ip::udp;

namespace fs = boost::filesystem;
namespace bt = bittorrent;

struct Client::Impl {
    // The newest protocol version number seen in a trusted exchange
    // (i.e. from injector-signed cached content).
    std::shared_ptr<unsigned> _newest_proto_seen;

    asio::executor _ex;
    shared_ptr<bt::MainlineDht> _dht;
    string _uri_swarm_prefix;
    util::Ed25519PublicKey _cache_pk;
    fs::path _cache_dir;
    Client::opt_path _static_cache_dir;
    unique_ptr<cache::HttpStore> _http_store;
    boost::posix_time::time_duration _max_cached_age;
    Cancel _lifetime_cancel;
    Announcer _announcer;
    map<string, udp::endpoint> _peer_cache;
    util::LruCache<std::string, shared_ptr<DhtLookup>> _dht_lookups;
    LocalPeerDiscovery _local_peer_discovery;
    std::unique_ptr<DhtGroups> _dht_groups;


    Impl( shared_ptr<bt::MainlineDht> dht_
        , util::Ed25519PublicKey& cache_pk
        , fs::path cache_dir
        , Client::opt_path static_cache_dir
        , unique_ptr<cache::HttpStore> http_store_
        , boost::posix_time::time_duration max_cached_age)
        : _newest_proto_seen(std::make_shared<unsigned>(http_::protocol_version_current))
        , _ex(dht_->get_executor())
        , _dht(move(dht_))
        , _uri_swarm_prefix(bep5::compute_uri_swarm_prefix
              (cache_pk, http_::protocol_version_current))
        , _cache_pk(cache_pk)
        , _cache_dir(move(cache_dir))
        , _static_cache_dir(std::move(static_cache_dir))
        , _http_store(move(http_store_))
        , _max_cached_age(max_cached_age)
        , _announcer(_dht)
        , _dht_lookups(256)
        , _local_peer_discovery(_ex, _dht->local_endpoints())
    {}

    std::string compute_swarm_name(boost::string_view dht_group) const {
        return bep5::compute_uri_swarm_name(
                _uri_swarm_prefix,
                dht_group);
    }

    shared_ptr<DhtLookup> dht_lookup(std::string swarm_name)
    {
        auto* lookup = _dht_lookups.get(swarm_name);

        if (!lookup) {
            lookup = _dht_lookups.put( swarm_name
                                     , make_shared<DhtLookup>(_dht, swarm_name));
        }

        return *lookup;
    }

    Session load( const std::string& key
                , const std::string& dht_group
                , bool is_head_request
                , Cancel cancel
                , Yield yield_)
    {
        Yield yield = yield_.tag("cache/client/load");

        namespace err = asio::error;

        sys::error_code ec;

        _YDEBUG(yield, "Requesting from the cache: ", key);

        bool rs_available = false;
        auto rs = load_from_local(key, is_head_request, cancel, yield[ec]);
        _YDEBUG(yield, "Looking up local cache; ec=", ec);
        if (ec == err::operation_aborted) return or_throw<Session>(yield, ec);
        if (!ec) {
            // TODO: Check its age, store it if it's too old but keep trying
            // other peers.
            auto rs_sz = _http_store->body_size(key, ec);
            if (ec) {
                _YERROR(yield, "Failed to get body size of response in local cache; ec=", ec);
                rs.close();
            } else {
                auto data_size_sv = rs.response_header()[http_::response_data_size_hdr];
                auto data_size_o = parse::number<std::size_t>(data_size_sv);
                if (data_size_o && rs_sz == *data_size_o)
                    return rs;  // local copy available and complete, use it
                rs_available = true;  // available but incomplete
                // TODO: Ideally, an incomplete or stale local cache entry
                // could be reused in the multi-peer download below.
            }
        }
        ec = {};  // try distributed cache

        string debug_tag;
        if (logger.get_threshold() <= DEBUG) {
            debug_tag = yield.tag() + "/multi_peer_reader";
        };

        auto reader = std::make_unique<MultiPeerReader>
            ( _ex
            , _cache_pk
            , _local_peer_discovery.found_peers()
            , key
            , _dht
            , dht_group
            , dht_lookup(compute_swarm_name(dht_group))
            , _newest_proto_seen
            , debug_tag);

        auto s = yield[ec].tag("read_hdr").run([&] (auto y) {
            return Session::create(std::move(reader), is_head_request, cancel, y);
        });

        if (!ec) {
            s.response_header().set( http_::response_source_hdr  // for agent
                                   , http_::response_source_hdr_dist_cache);
        } else if (ec != err::operation_aborted && rs_available) {
            _YDEBUG(yield, "Multi-peer session creation failed, falling back to incomplete local copy;"
                    " ec=", ec);
            // Do not use `.set` as several warnings may co-exist
            // (RFC7234#5.5).
            rs.response_header().insert( http::field::warning
                                       , "119 Ouinet \"Using incomplete response body from local cache\"");
            return rs;
        }

        return or_throw<Session>(yield, ec, move(s));
    }

    Session load_from_local( const std::string& key
                           , bool is_head_request
                           , Cancel cancel
                           , Yield yield)
    {
        sys::error_code ec;
        auto rr = _http_store->reader(key, ec);
        if (ec) return or_throw<Session>(yield, ec);
        auto rs = yield[ec].tag("read_hdr").run([&] (auto y) {
            return Session::create(move(rr), is_head_request, cancel, y);
        });
        assert(!cancel || ec == asio::error::operation_aborted);
        if (!ec) rs.response_header().set( http_::response_source_hdr  // for agent
                                         , http_::response_source_hdr_local_cache);
        return or_throw(yield, ec, move(rs));
    }

    void store( const std::string& key
              , const std::string& dht_group
              , http_response::AbstractReader& r
              , Cancel cancel
              , asio::yield_context yield)
    {
        sys::error_code ec;
        cache::KeepSignedReader fr(r);
        _http_store->store(key, fr, cancel, yield[ec]);
        if (ec) return or_throw(yield, ec);

        _dht_groups->add(dht_group, key, cancel, yield[ec]);
        if (ec) return or_throw(yield, ec);

        _announcer.add(compute_swarm_name(dht_group));
    }

    http::response_header<>
    read_response_header( http_response::AbstractReader& reader
                        , asio::yield_context yield)
    {
        Cancel lc(_lifetime_cancel);

        sys::error_code ec;
        auto part = reader.async_read_part(lc, yield[ec]);
        if (!ec && !part)
            ec = sys::errc::make_error_code(sys::errc::no_message);
        return_or_throw_on_error(yield, lc, ec, http::response_header<>());
        auto head = part->as_head(); assert(head);
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
    void unpublish_cache_entry(const std::string& key)
    {
        auto empty_groups = _dht_groups->remove(key);
        for (const auto& eg : empty_groups) _announcer.remove(compute_swarm_name(eg));
    }

    // Return whether the entry should be kept in storage.
    bool keep_cache_entry(cache::reader_uptr rr, asio::yield_context yield)
    {
        // This should be available to
        // allow removing keys of entries to be evicted.
        assert(_dht_groups);

        sys::error_code ec;

        auto hdr = read_response_header(*rr, yield[ec]);
        if (ec) return or_throw<bool>(yield, ec);

        if (hdr[http_::protocol_version_hdr] != http_::protocol_version_hdr_current) {
            _WARN( "Cached response contains an invalid "
                 , http_::protocol_version_hdr
                 , " header field; removing");
            return false;
        }

        auto key = hdr[http_::response_uri_hdr];
        if (key.empty()) {
            _WARN( "Cached response does not contain a "
                 , http_::response_uri_hdr
                 , " header field; removing");
            return false;
        }

        auto age = cache_entry_age(hdr);
        if (age > _max_cached_age) {
            _DEBUG( "Cached response is too old; removing: "
                  , age, " > ", _max_cached_age
                  , "; uri=", key );
            unpublish_cache_entry(key.to_string());
            return false;
        }

        return true;
    }

    void announce_stored_data(asio::yield_context y)
    {
        static const auto groups_curver_subdir = "dht_groups";

        Cancel cancel(_lifetime_cancel);

        sys::error_code e;

        // Use static DHT groups if its directory is provided.
        std::unique_ptr<BaseDhtGroups> static_dht_groups;
        if (_static_cache_dir) {
            auto groups_dir = *_static_cache_dir / groups_curver_subdir;
            if (!is_directory(groups_dir)) {
                _ERROR("No DHT groups of supported version under static cache, ignoring: ", *_static_cache_dir);
            } else {
                static_dht_groups = load_static_dht_groups(move(groups_dir), _ex, cancel, y[e]);
                if (e) _ERROR("Failed to load static DHT groups, ignoring: ", *_static_cache_dir);
            }
        }

        auto groups_dir = _cache_dir / groups_curver_subdir;
        _dht_groups = static_dht_groups
            ? load_backed_dht_groups(groups_dir, move(static_dht_groups), _ex, cancel, y[e])
            : load_dht_groups(groups_dir, _ex, cancel, y[e]);

        if (cancel) e = asio::error::operation_aborted;
        if (e) return or_throw(y, e);

        _http_store->for_each([&] (auto rr, auto yield) {
            return keep_cache_entry(std::move(rr), yield);
        }, cancel, y[e]);
        if (e) return or_throw(y, e);

        // These checks are not bullet-proof, but they should catch some inconsistencies
        // between resource groups and the HTTP store.
        std::set<DhtGroups::ItemName> bad_items;
        std::set<DhtGroups::GroupName> bad_groups;
        for (auto& group_name : _dht_groups->groups()) {
            unsigned good_items = 0;
            for (auto& group_item : _dht_groups->items(group_name)) {
                // TODO: This implies opening all cache items (again for local cache), make lighter.
                sys::error_code ec;
                if (_http_store->reader(group_item, ec) != nullptr)
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
            _dht_groups->remove_group(group_name);
        for (auto& item_name : bad_items)
            _dht_groups->remove(item_name);

        // Finally, announce the standing groups.
        for (auto& group_name : _dht_groups->groups())
            _announcer.add(compute_swarm_name(group_name));
    }

    void stop() {
        _lifetime_cancel();
        _local_peer_discovery.stop();
    }

    unsigned get_newest_proto_version() const {
        return *_newest_proto_seen;
    }
};

/* static */
std::unique_ptr<Client>
Client::build( shared_ptr<bt::MainlineDht> dht
             , util::Ed25519PublicKey cache_pk
             , fs::path cache_dir
             , boost::posix_time::time_duration max_cached_age
             , Client::opt_path static_cache_dir
             , Client::opt_path static_cache_content_dir
             , asio::yield_context yield)
{
    using ClientPtr = unique_ptr<Client>;
    static const auto store_oldver_subdirs = {"data", "data-v1", "data-v2"};
    static const auto store_curver_subdir = "data-v3";

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
            static_http_store = make_static_http_store( move(store_dir)
                                                      , move(canon_content_dir)
                                                      , cache_pk
                                                      , dht->get_executor());
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
    if (ec) return or_throw<ClientPtr>(yield, ec);
    auto http_store = static_http_store
        ? make_backed_http_store(move(store_dir), move(static_http_store), dht->get_executor())
        : make_http_store(move(store_dir), dht->get_executor());

    unique_ptr<Impl> impl(new Impl( move(dht)
                                  , cache_pk, move(cache_dir), std::move(static_cache_dir)
                                  , move(http_store), max_cached_age));

    impl->announce_stored_data(yield[ec]);
    if (ec) return or_throw<ClientPtr>(yield, ec);

    return unique_ptr<Client>(new Client(move(impl)));
}

Client::Client(unique_ptr<Impl> impl)
    : _impl(move(impl))
{}

Session Client::load( const std::string& key
                    , const std::string& dht_group
                    , bool is_head_request
                    , Cancel cancel, Yield yield)
{
    return _impl->load(key, dht_group, is_head_request, cancel, yield);
}

void Client::store( const std::string& key
                  , const std::string& dht_group
                  , http_response::AbstractReader& r
                  , Cancel cancel
                  , asio::yield_context yield)
{
    _impl->store(key, dht_group, r, cancel, yield);
}

unsigned Client::get_newest_proto_version() const
{
    return _impl->get_newest_proto_version();
}

Client::~Client()
{
    _impl->stop();
}
