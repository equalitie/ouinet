#pragma once

#include <functional>
#include <set>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>

#include "../../response_reader.h"
#include "../../util/crypto.h"
#include "../../util/yield.h"
#include "cache_entry.h"
#include "dht_groups.h"

namespace ouinet {

class Session;

namespace cache {

class LocalClient {
private:
    struct Impl;
    using opt_path = boost::optional<fs::path>;

    static std::unique_ptr<LocalClient>
    build( asio::executor exec
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , opt_path static_cache_dir
         , opt_path static_cache_content_dir
         , asio::yield_context);

public:
    using GroupName = BaseDhtGroups::GroupName;
    using GroupRemoveHook = std::function<void (const GroupName&)>;

public:
    static std::unique_ptr<LocalClient>
    build( asio::executor exec
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , asio::yield_context yield)
    {
        return build( std::move(exec), std::move(cache_pk)
                    , std::move(cache_dir), max_cached_age
                    , boost::none, boost::none
                    , yield);
    }

    static std::unique_ptr<LocalClient>
    build( asio::executor exec
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , boost::posix_time::time_duration max_cached_age
         , fs::path static_cache_dir
         , fs::path static_cache_content_dir
         , asio::yield_context yield)
    {
        assert(!static_cache_dir.empty());
        assert(!static_cache_content_dir.empty());
        return build( std::move(exec), std::move(cache_pk)
                    , std::move(cache_dir), max_cached_age
                    , opt_path{std::move(static_cache_dir)}
                    , opt_path{std::move(static_cache_content_dir)}
                    , yield);
    }

    // Use to call the given hook when a group is removed.
    //
    // The previous hook is returned.
    GroupRemoveHook on_group_remove(GroupRemoveHook);

    // Remove the hook called when a group is removed.
    //
    // The previous hook is returned.
    GroupRemoveHook on_group_remove();

    // This may add a response source header.
    //
    // If the operation is successful and the request is not just for the head,
    // `is_complete` is changed to indicate
    // whether the response body in the store is complete.
    Session load( const std::string& key
                , const GroupName& group
                , bool is_head_request
                , bool& is_complete
                , Cancel
                , Yield);

    void store( const std::string& key
              , const GroupName& group
              , http_response::AbstractReader&
              , Cancel
              , asio::yield_context);

    // Returns true if both request and response had keep-alive == true.
    // Times out if forwarding to the sink gets stuck.
    bool serve( const http::request<http::empty_body>&
              , GenericStream& sink
              , Cancel&
              , Yield);

    std::size_t size(Cancel, asio::yield_context) const;

    void purge(Cancel, asio::yield_context);

    // Get all groups being present in this client.
    std::set<GroupName> get_groups() const;

    ~LocalClient();

private:
    LocalClient(std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> _impl;
};

}} // namespaces
