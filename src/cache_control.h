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

class GenericStream;

class CacheControl {
private:
    struct FetchState;

public:
    using DhtGroup = std::string;
    using Request  = http::request<http::string_body>;
    using Response = http::response<http::dynamic_body>;

    using FetchStored = std::function<CacheEntry(const Request&, const DhtGroup&, Cancel&, Yield)>;
    using FetchFresh  = std::function<Session(const Request&, Cancel&, Yield)>;

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

    void max_cached_age(const boost::posix_time::time_duration&);
    boost::posix_time::time_duration max_cached_age() const;

    // Aggressive caching allows storing a response regardless of
    // being private or the result of an authorized request
    // (in spite of Section 3 of RFC 7234).
    static bool ok_to_cache( const http::request_header<>&  request
                           , const http::response_header<>& response
                           , bool aggressive_cache = false
                           , const char** reason = nullptr);

    void enable_parallel_fetch(bool value) {
        _parallel_fetch_enabled = value;
    }

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

    Session do_fetch_fresh(FetchState&, const Request&, Yield);
    CacheEntry do_fetch_stored( FetchState&
                              , const Request&
                              , const boost::optional<DhtGroup>&
                              , bool& is_fresh
                              , Yield);

    //bool is_stale( const boost::posix_time::ptime& time_stamp
    //             , const Response&) const;

    bool is_older_than_max_cache_age(const boost::posix_time::ptime&) const;

    auto make_fetch_fresh_job(const Request&, Yield&);

    bool has_temporary_result(const Session&) const;

private:
    asio::executor _ex;
    std::string _server_name;
    bool _parallel_fetch_enabled = true;

    boost::posix_time::time_duration _max_cached_age
        = default_max_cached_age;
};

} // ouinet namespace
