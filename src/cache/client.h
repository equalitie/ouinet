#pragma once

#include <set>

#include <boost/asio/ip/udp.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>

#include "../bittorrent/dht.h"
#include "../response_reader.h"
#include "../util/crypto.h"
#include "../util/yield.h"
#include "cache_entry.h"
#include "dht_groups.h"


namespace ouinet {

namespace bittorrent {
    class MainlineDht;
}

class Session;

namespace cache {

class Client {
private:
    struct Impl;
    using opt_path = boost::optional<fs::path>;

    static std::unique_ptr<Client>
    build( AsioExecutor ex
         , std::set<asio::ip::udp::endpoint> lan_my_endpoints
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , opt_path static_cache_dir
         , opt_path static_cache_content_dir
         , asio::yield_context);

public:
    using GroupName = BaseDhtGroups::GroupName;

public:
    static std::unique_ptr<Client>
    build( AsioExecutor ex
         , std::set<asio::ip::udp::endpoint> lan_my_endpoints
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , asio::yield_context yield)
    {
        return build( ex, std::move(lan_my_endpoints), std::move(cache_pk)
                    , std::move(cache_dir), max_cached_age
                    , boost::none, boost::none
                    , yield);
    }

    static std::unique_ptr<Client>
    build( AsioExecutor ex
         , std::set<asio::ip::udp::endpoint> lan_my_endpoints
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , fs::path static_cache_dir
         , fs::path static_cache_content_dir
         , asio::yield_context yield)
    {
        assert(!static_cache_dir.empty());
        assert(!static_cache_content_dir.empty());
        return build( ex, std::move(lan_my_endpoints), std::move(cache_pk)
                    , std::move(cache_dir), max_cached_age
                    , opt_path{std::move(static_cache_dir)}
                    , opt_path{std::move(static_cache_content_dir)}
                    , yield);
    }

    // Returns true the first time the DHT is successfully enabled,
    // false otherwise.
    bool enable_dht(std::shared_ptr<bittorrent::MainlineDht>, size_t simultaneous_announcements);


    // This may add a response source header.
    Session load( const std::string& key
                , const GroupName& group
                , bool is_head_request
                , metrics::Client& metrics
                , Cancel
                , Yield);

    void store( const std::string& key
              , const GroupName& group
              , http_response::AbstractReader&
              , Cancel
              , asio::yield_context);

    // Returns true if both request and response had keep-alive == true.
    // Times out if forwarding to the sink gets stuck.
    bool serve_local( const http::request<http::empty_body>&
                    , GenericStream& sink
                    , metrics::Client&
                    , Cancel&
                    , Yield);

    std::size_t local_size( Cancel
                          , asio::yield_context) const;

    void local_purge( Cancel
                    , asio::yield_context);
    void pin_group(const std::string& group_name);
    void unpin_group(const std::string& group_name);

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
