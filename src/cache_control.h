#pragma once

#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/http.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "util/yield.h"
#include "cache/cache_entry.h"
#include "namespaces.h"

namespace ouinet {

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
    using DhtGroup = std::string;
    using Request  = http::request<http::string_body>;
    using Response = http::response<http::dynamic_body>;

    using FetchStored = std::function<CacheEntry(const Request&, const DhtGroup&, Cancel&, Yield)>;
    // If not null, the given cache entry is already available
    // (e.g. this may be a revalidation).
    using FetchFresh  = std::function<Session(const Request&, const CacheEntry*, Cancel&, Yield)>;
    // When fetching stored (which may be slow), a parallel request to fetch fresh is started
    // only if this is not null and it returns true.
    using ParallelFresh = std::function<bool(const Request&, const boost::optional<DhtGroup>&)>;

public:
    CacheControl(const asio::executor& ex, std::string server_name)
        : _ex(ex)
        , _server_name(std::move(server_name))
    {}

    CacheControl(asio::io_context& ctx, std::string server_name)
        : _ex(ctx.get_executor())
        , _server_name(std::move(server_name))
    {}

    Session fetch(const Request&,
                  const boost::optional<DhtGroup>&,
                  sys::error_code& fresh_ec,
                  sys::error_code& cache_ec,
                  Cancel&,
                  Yield);

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
            const Request&,
            const boost::optional<DhtGroup>&,
            sys::error_code& fresh_ec,
            sys::error_code& cache_ec,
            Cancel&,
            Yield);

    Session do_fetch_fresh( FetchState&, const Request&, const CacheEntry*, Yield);
    CacheEntry do_fetch_stored( FetchState&
                              , const Request&
                              , const boost::optional<DhtGroup>&
                              , bool& is_fresh
                              , Yield);

    //bool is_stale( const boost::posix_time::ptime& time_stamp
    //             , const Response&) const;

    bool is_older_than_max_cache_age(const boost::posix_time::ptime&) const;

    auto make_fetch_fresh_job(const Request&, const CacheEntry*, Yield);

    bool has_temporary_result(const Session&) const;

private:
    asio::executor _ex;
    std::string _server_name;

    boost::posix_time::time_duration _max_cached_age
        = default_max_cached_age;
};

} // ouinet namespace
