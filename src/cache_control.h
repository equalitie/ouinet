#pragma once

#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/http.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "util/yield.h"
#include "cache/cache_entry.h"
#include "request.h"
#include "namespaces.h"

namespace ouinet {
using ouinet::util::AsioExecutor;

static const boost::posix_time::time_duration default_max_cached_age
    = boost::posix_time::hours(7 * 24);  // one week

// Announcements are processed one at a time in Android to avoid increasing battery usage
#ifdef __ANDROID__
    const size_t default_max_simultaneous_announcements = 1;
#else
    const size_t default_max_simultaneous_announcements = 16;
#endif

class GenericStream;

class CacheControl {
private:
    struct FetchState;

public:
    using Response = http::response<http::dynamic_body>;

    using FetchStored = std::function<CacheEntry(const CacheRequest&, Cancel&, YieldContext)>;
    // If not null, the given cache entry is already available
    // (e.g. this may be a revalidation).
    using FetchFresh  = std::function<Session(const CacheRequest&, const CacheEntry*, Cancel&, YieldContext)>;
    // When fetching stored (which may be slow), a parallel request to fetch fresh is started
    // only if this is not null and it returns true.
    using ParallelFresh = std::function<bool(const CacheRequest&)>;

public:
    CacheControl(const AsioExecutor& ex, std::string server_name)
        : _ex(ex)
        , _server_name(std::move(server_name))
    {}

    CacheControl(asio::io_context& ctx, std::string server_name)
        : _ex(ctx.get_executor())
        , _server_name(std::move(server_name))
    {}

    Session fetch(const CacheRequest&,
                  sys::error_code& fresh_ec,
                  sys::error_code& cache_ec,
                  Cancel&,
                  YieldContext);

    FetchStored  fetch_stored;
    FetchFresh   fetch_fresh;
    ParallelFresh parallel_fresh;

    void max_cached_age(const boost::posix_time::time_duration&);
    boost::posix_time::time_duration max_cached_age() const;

    // Private caching allows storing a response regardless of
    // being private or the result of an authorized request
    // (in spite of Section 3 of RFC 7234).
    static bool ok_to_cache( const http::request_header<>&  request
                           , const http::response_header<>& response
                           , bool cache_private = false
                           , const char** reason = nullptr);

    static
    bool is_expired( const http::response_header<>&
                   , boost::posix_time::ptime time_stamp);

    static
    bool is_expired(const CacheEntry&);

private:
    Session do_fetch(
            const CacheRequest&,
            sys::error_code& fresh_ec,
            sys::error_code& cache_ec,
            Cancel&,
            YieldContext);

    Session do_fetch_fresh( FetchState&, const CacheRequest&, const CacheEntry*, YieldContext);

    CacheEntry do_fetch_stored( FetchState&
                              , const CacheRequest&
                              , bool& is_fresh
                              , YieldContext);

    //bool is_stale( const boost::posix_time::ptime& time_stamp
    //             , const Response&) const;

    bool is_older_than_max_cache_age(const boost::posix_time::ptime&) const;

    auto make_fetch_fresh_job(const CacheRequest&, const CacheEntry*, YieldContext);

    bool has_temporary_result(const Session&) const;

private:
    AsioExecutor _ex;
    std::string _server_name;

    boost::posix_time::time_duration _max_cached_age
        = default_max_cached_age;
};

} // ouinet namespace
