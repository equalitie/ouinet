#pragma once

#include <boost/beast/http.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "namespaces.h"

namespace ouinet {

class CacheControl {
public:
    using Request  = http::request<http::string_body>;
    using Response = http::response<http::dynamic_body>;

    struct CacheEntry {
        boost::posix_time::ptime time_stamp;
        Response response;
    };

    using FetchStored = std::function<CacheEntry(const Request&,
            asio::yield_context)>;

    using FetchFresh = std::function<Response(const Request&,
            asio::yield_context)>;

public:
    Response fetch(const Request&, asio::yield_context);

    FetchStored  fetch_stored;
    FetchFresh   fetch_fresh;

    void max_cached_age(const boost::posix_time::time_duration&);
    boost::posix_time::time_duration max_cached_age() const;

private:
    Response do_fetch(const Request&, asio::yield_context);
    Response do_fetch_fresh(const Request&, asio::yield_context);
    CacheEntry do_fetch_stored(const Request&, asio::yield_context);

    bool is_stale( const boost::posix_time::ptime& time_stamp
                 , const Response&) const;

    bool is_older_than_max_cache_age(const boost::posix_time::ptime&) const;

private:
    boost::posix_time::time_duration _max_cached_age
        = boost::posix_time::hours(7*24);  // one week
};

} // ouinet namespace
