#pragma once

#include "../../response_reader.h"
#include "../../util/crypto.h"
#include "../../util/yield.h"
#include "cache_entry.h"
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <set>

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
    build( std::shared_ptr<bittorrent::MainlineDht>
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , opt_path static_cache_dir
         , opt_path static_cache_content_dir
         , asio::yield_context);

public:
    static std::unique_ptr<Client>
    build( std::shared_ptr<bittorrent::MainlineDht> dht
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , asio::yield_context yield)
    {
        return build( std::move(dht), std::move(cache_pk)
                    , std::move(cache_dir), max_cached_age
                    , boost::none, boost::none
                    , yield);
    }

    static std::unique_ptr<Client>
    build( std::shared_ptr<bittorrent::MainlineDht> dht
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , fs::path static_cache_dir
         , fs::path static_cache_content_dir
         , asio::yield_context yield)
    {
        assert(!static_cache_dir.empty());
        assert(!static_cache_content_dir.empty());
        return build( std::move(dht), std::move(cache_pk)
                    , std::move(cache_dir), max_cached_age
                    , opt_path{std::move(static_cache_dir)}
                    , opt_path{std::move(static_cache_content_dir)}
                    , yield);
    }


    // This may add a response source header.
    Session load( const std::string& key
                , const std::string& dht_group
                , bool is_head_request
                , Cancel
                , Yield);

    void store( const std::string& key
              , const std::string& dht_group
              , http_response::AbstractReader&
              , Cancel
              , asio::yield_context);

    // Returns true if both request and response had keep-alive == true.
    // Times out if forwarding to the sink gets stuck.
    bool serve_local( const http::request<http::empty_body>&
                    , GenericStream& sink
                    , Cancel&
                    , Yield);

    std::size_t local_size( Cancel
                          , asio::yield_context) const;

    void local_purge( Cancel
                    , asio::yield_context);

    // Get the newest protocol version that has been seen in the network
    // (e.g. to warn about potential upgrades).
    unsigned get_newest_proto_version() const;

    // Get all groups being announced to the distributed cache index
    // by this client.
    std::set<std::string> get_announced_groups() const;
  
    ~Client();

private:
    Client(std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> _impl;
};

}} // namespaces
