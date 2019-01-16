#pragma once

#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/http.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "util/yield.h"
#include "cache/cache_entry.h"
#include "namespaces.h"

namespace ouinet {

class CacheControl {
private:
    struct FetchState;

public:
    using Request  = http::request<http::string_body>;
    using Response = http::response<http::dynamic_body>;

    using FetchStored = std::function<CacheEntry(const Request&, Cancel&, Yield)>;
    using FetchFresh  = std::function<Response(const Request&, Cancel&, Yield)>;
    // This function may alter a (moved) response and return it.
    using Store = std::function<Response(const Request&, Response, Cancel&, Yield)>;

public:
    CacheControl(asio::io_service& ios, std::string server_name)
        : _ios(ios)
        , _server_name(std::move(server_name))
    {}

    Response fetch(const Request&, Cancel&, Yield);

    FetchStored  fetch_stored;
    FetchFresh   fetch_fresh;
    Store        store;

    Response try_to_cache(const Request&, Response, Yield) const;

    void max_cached_age(const boost::posix_time::time_duration&);
    boost::posix_time::time_duration max_cached_age() const;

    // Returns ptime() if parsing fails.
    static boost::posix_time::ptime parse_date(beast::string_view);

    static bool ok_to_cache( const http::request_header<>&  request
                           , const http::response_header<>& response
                           , const char** reason = nullptr);

    void enable_parallel_fetch(bool value) {
        _parallel_fetch_enabled = value;
    }

private:
    // TODO: Add cancellation support
    Response do_fetch(const Request&, Cancel&, Yield);
    Response do_fetch_fresh(FetchState&, const Request&, Yield);
    CacheEntry do_fetch_stored(FetchState&, const Request&, Yield);

    bool is_stale( const boost::posix_time::ptime& time_stamp
                 , const Response&) const;

    bool is_older_than_max_cache_age(const boost::posix_time::ptime&) const;

    Response bad_gateway(const Request&, beast::string_view reason);

    auto make_fetch_fresh_job(const Request&, Yield&);

    bool has_temporary_result(const Response&) const;

private:
    asio::io_service& _ios;
    std::string _server_name;
    bool _parallel_fetch_enabled = true;

    boost::posix_time::time_duration _max_cached_age
        = boost::posix_time::hours(7*24);  // one week
};

} // ouinet namespace
