#include "client.h"
#include "announcer.h"
#include "dht_lookup.h"
#include "local_client.h"
#include "local_peer_discovery.h"
#include "http_sign.h"
#include "../http_util.h"
#include "../util/lru_cache.h"
#include "../bittorrent/dht.h"
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

namespace bt = bittorrent;

struct Client::Impl {
    // The newest protocol version number seen in a trusted exchange
    // (i.e. from injector-signed cached content).
    std::shared_ptr<unsigned> _newest_proto_seen;

    asio::executor _ex;
    shared_ptr<bt::MainlineDht> _dht;
    string _uri_swarm_prefix;
    util::Ed25519PublicKey _cache_pk;
    std::shared_ptr<LocalClient> _local_client;
    Cancel _lifetime_cancel;
    Announcer _announcer;
    map<string, udp::endpoint> _peer_cache;
    util::LruCache<std::string, shared_ptr<DhtLookup>> _dht_lookups;
    LocalPeerDiscovery _local_peer_discovery;
    std::unique_ptr<DhtGroups> _dht_groups;


    Impl( shared_ptr<bt::MainlineDht> dht_
        , util::Ed25519PublicKey& cache_pk
        , std::shared_ptr<LocalClient> local_client)
        : _newest_proto_seen(std::make_shared<unsigned>(http_::protocol_version_current))
        , _ex(dht_->get_executor())
        , _dht(move(dht_))
        , _uri_swarm_prefix(bep5::compute_uri_swarm_prefix
              (cache_pk, http_::protocol_version_current))
        , _cache_pk(cache_pk)
        , _local_client(move(local_client))
        , _announcer(_dht)
        , _dht_lookups(256)
        , _local_peer_discovery(_ex, _dht->local_endpoints())
    {
        // Finally, hook into group removal from the local cache
        // in order to stop announcing dropped groups to the DHT,
        // and announce all groups currently in the local cache.
        _local_client->on_group_remove([&] (auto& group_name) {
            _announcer.remove(group_name);
        });

        for (auto& group_name : _local_client->get_groups())
            _announcer.add(compute_swarm_name(group_name));
    }

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

        bool rs_available = false, is_complete;
        auto rs = _local_client->load(key, dht_group, is_head_request, is_complete, cancel, yield[ec]);
        _YDEBUG(yield, "Looking up local cache; ec=", ec);
        if (ec == err::operation_aborted) return or_throw<Session>(yield, ec);
        if (!ec) {
            // TODO: Check its age, store it if it's too old but keep trying
            // other peers.
            if (is_head_request || is_complete)
                return rs;  // local copy available and complete, use it
            rs_available = true;  // available but incomplete
            // TODO: Ideally, an incomplete or stale local cache entry
            // could be reused in the multi-peer download below.
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

    void store( const std::string& key
              , const std::string& dht_group
              , http_response::AbstractReader& r
              , Cancel cancel
              , asio::yield_context yield)
    {
        sys::error_code ec;
        _local_client->store(key, dht_group, r, cancel, yield[ec]);
        if (ec) return or_throw(yield, ec);

        _announcer.add(compute_swarm_name(dht_group));
    }

    void stop() {
        _lifetime_cancel();
        _local_peer_discovery.stop();
        _local_client->on_group_remove();
    }

    unsigned get_newest_proto_version() const {
        return *_newest_proto_seen;
    }
};

/* static */
std::unique_ptr<Client>
Client::build( shared_ptr<bt::MainlineDht> dht
             , util::Ed25519PublicKey cache_pk
             , std::shared_ptr<LocalClient> local_client
             , asio::yield_context yield)
{
    unique_ptr<Impl> impl(new Impl( move(dht)
                                  , cache_pk
                                  , move(local_client)));

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
