#pragma once

#include <functional>
#include <utility>
#include <vector>
#include <deque>

#include <boost/asio/error.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/http.hpp>
#include <boost/regex.hpp>

#include "namespaces.h"
#include "http_util.h"


namespace ouinet {

//------------------------------------------------------------------------------
namespace request_route {

// TODO: It may make sense to split private/dynamic/non-cached channels (origin, proxy)
// from public/static/cached channels (cache/injector)
// so that channels of different types cannot be mixed,
// i.e. it makes no sense to attempt a request which was considered private
// over a public channel like cache or injector,
// and similarly sending a public request to the origin
// misses the opportunity to use the cache for it.
enum class fresh_channel {
    // These channels may be configured by the user.
    secure_origin,      // send request to the origin HTTP server while forcing TLS
    origin,             // send request to the origin HTTP server as is (with or without TLS)
    proxy,              // send request to proxy ouiservice
    injector_or_dcache, // send request to injector ouiservice
    _front_end,         // handle the request internally
};

// A request router configuration will be
// chosen by the client when receiving a request and
// considered when serving calls from the cache control to
// fetch fresh or cached content, or to cache it.
struct Config {
    // When the cache control decides that the request may be so fullfilled,
    // enable looking up a cached response.
    bool enable_stored = true;
    // When the cache control decides that a fresh response is needed,
    // attempt those channels in order until one succeeds.
    // If it was the Injector channel, the response may get cached.
    std::deque<fresh_channel> fresh_channels;
};
} // request_route namespace

//------------------------------------------------------------------------------
// Request expressions can tell whether they match a given request
// (much like regular expressions match strings).
namespace reqexpr {
class ReqExpr;

// The type of functions that retrieve a given field (as a string) from a request.
using field_getter = std::function<beast::string_view (const http::request<http::string_body>&)>;

class reqex {
    friend reqex true_();
    friend reqex false_();
    friend reqex from_regex(const field_getter&, const boost::regex&);
    friend reqex operator!(const reqex&);
    friend reqex operator&&(const reqex&, const reqex&);
    friend reqex operator||(const reqex&, const reqex&);

    private:
        const std::shared_ptr<ReqExpr> impl;
        reqex(const std::shared_ptr<ReqExpr> impl_) : impl(impl_) { }

    public:
        // True when the request matches this expression.
        bool match(const http::request<http::string_body>& req) const;
};

// Use the following functions to create request expressions,
// then call the ``match()`` method of the resulting object
// with the request that you want to check.

// Always matches, regardless of request content.
reqex true_();
// Never matches, regardless of request content.
reqex false_();
// Only matches when the extracted field matches the given (anchored) regular expression.
reqex from_regex(const field_getter&, const boost::regex&);
reqex from_regex(const field_getter&, const std::string&);

// Negates the matching of the given expression.
reqex operator!(const reqex&);

// Short-circuit AND and OR operations on the given expressions.
reqex operator&&(const reqex&, const reqex&);
reqex operator||(const reqex&, const reqex&);
} // ouinet::reqexpr namespace


//------------------------------------------------------------------------------
namespace request_route {
// Route the provided request according to the list of channels associated
// with the first matching expression in the given list,
// otherwise route it according to the given list of default channels.
const Config&
route_choose_config( const http::request<http::string_body>& req
                   , const std::vector<std::pair<const reqexpr::reqex, const Config>>& matches
                   , const Config& default_config );
} // request_route namespace
//------------------------------------------------------------------------------

} // ouinet namespace
