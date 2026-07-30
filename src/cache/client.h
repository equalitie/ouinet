#pragma once

#include <set>

#include <boost/asio/ip/udp.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>

#include "../bittorrent/mainline_dht.h"
#include "../response_reader.h"
#include "../util/sign.h"
#include "resource_id.h"
#include "dht_groups.h"
#include "peer_message.h"
#include "util/crypto_stream_key.h"
#include "ouiservice/i2p/fwd.h"
#include "ouiservice/i2p/address.h"

namespace ouinet {

namespace bittorrent {
    class DhtBase;
}

class Session;
class Async;
class CachePeerRetrieveRequest;

namespace cache {

class Client {
private:
    struct Impl;
    using opt_path = boost::optional<fs::path>;

    [[nodiscard]]
    static std::expected<std::shared_ptr<Client>, sys::error_code>
    build( std::set<asio::ip::udp::endpoint> lan_my_endpoints
         , sign::PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , opt_path static_cache_dir
         , opt_path static_cache_content_dir
         , Async);

public:
    using GroupName = BaseDhtGroups::GroupName;

public:
    [[nodiscard]]
    static std::expected<std::shared_ptr<Client>, sys::error_code>
    build( std::set<asio::ip::udp::endpoint> lan_my_endpoints
         , sign::PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , Async yield)
    {
        return build( std::move(lan_my_endpoints), std::move(cache_pk)
                    , std::move(cache_dir), max_cached_age
                    , boost::none, boost::none
                    , yield);
    }

    [[nodiscard]]
    static std::expected<std::shared_ptr<Client>, sys::error_code>
    build( std::set<asio::ip::udp::endpoint> lan_my_endpoints
         , sign::PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , fs::path static_cache_dir
         , fs::path static_cache_content_dir
         , Async yield)
    {
        assert(!static_cache_dir.empty());
        assert(!static_cache_content_dir.empty());
        return build( std::move(lan_my_endpoints), std::move(cache_pk)
                    , std::move(cache_dir), max_cached_age
                    , opt_path{std::move(static_cache_dir)}
                    , opt_path{std::move(static_cache_content_dir)}
                    , yield);
    }

    // Returns true the first time the DHT is successfully enabled,
    // false otherwise.
    bool enable_dht(std::shared_ptr<bittorrent::DhtBase>, size_t simultaneous_announcements);

    // Returns true the first time the I2P is successfully enabled,
    // false otherwise.
    bool enable_i2p(std::shared_ptr<I2pSession>, I2pAddress tracker_addr);

    // This may add a response source header.
    [[nodiscard]]
    std::expected<Session, sys::error_code>
    load( const CachePeerRetrieveRequest&
        , metrics::Client& metrics
        , Async);

    [[nodiscard]]
    std::expected<void, sys::error_code>
    store( const ResourceId&
         , const GroupName& group
         , http_response::AbstractReader&
         , Async);

    // Returns true if both request and response had keep-alive == true.
    // Times out if forwarding to the sink gets stuck.
    [[nodiscard]]
    std::expected<void, sys::error_code>
    serve_local( const PeerCacheRequest&
               , GenericStream& sink
               , metrics::Client&
               , Async);

    [[nodiscard]]
    std::expected<std::size_t, sys::error_code> local_size(Async) const;

    [[nodiscard]] std::expected<void, sys::error_code> local_purge(Async);

    bool pin_group(const std::string& group_name, sys::error_code& ec);
    bool unpin_group(const std::string& group_name, sys::error_code&);
    bool is_pinned_group(const std::string& group_name, sys::error_code&);

    // Get the newest protocol version that has been seen in the network
    // (e.g. to warn about potential upgrades).
    unsigned get_newest_proto_version() const;

    // Get all groups present in this client.
    std::set<GroupName> get_groups() const;
    std::set<Client::GroupName> get_pinned_groups();

    ~Client();

private:
    Client(std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> _impl;
};

}} // namespaces
