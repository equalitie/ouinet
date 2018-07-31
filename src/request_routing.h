#pragma once

#include <functional>
#include <utility>
#include <vector>
#include <queue>

#include <boost/asio/error.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/http.hpp>
#include <boost/regex.hpp>

#include "namespaces.h"


namespace ouinet {
// The presence of this (non-empty) HTTP request header
// shows the protocol version used by the client
// and hints the receiving injector to behave like an injector instead of a proxy.
static const std::string request_version_hdr = "X-Ouinet-Version";
static const std::string request_version_hdr_v0 = "0";
static const std::string request_version_hdr_latest = request_version_hdr_v0;

//------------------------------------------------------------------------------
namespace request_route {

// TODO: Better name?
//
// TODO: It may make sense to split private/dynamic/non-cached mechanisms (origin, proxy)
// from public/static/cached mechanisms (cache/injector)
// so that mechanisms of different types cannot be mixed,
// i.e. it makes no sense to attempt a request which was considered private
// over a public mechanism like cache or injector,
// and similarly sending a public request to the origin
// misses the opportunity to use the cache for it.
enum class responder {
    // These mechanisms may be configured by the user.
    origin,      // send request to the origin HTTP server
    proxy,       // send request to proxy ouiservice
    injector,    // send request to injector ouiservice
    _front_end,  // handle the request internally
};

struct Config {
    bool enable_cache = true;
    std::queue<responder> responders;
};

std::ostream& operator<<(std::ostream&, responder);
std::ostream& operator<<(std::ostream&, const Config&);

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
// Route the provided request according to the list of mechanisms associated
// with the first matching expression in the given list,
// otherwise route it according to the given list of default mechanisms.
const Config&
route_choose_config( const http::request<http::string_body>& req
                   , const std::vector<std::pair<const reqexpr::reqex, const Config>>& matches
                   , const Config& default_config );
} // request_route namespace
//------------------------------------------------------------------------------

} // ouinet namespace
