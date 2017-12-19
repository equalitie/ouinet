#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <boost/beast/core/string.hpp>
#include <boost/beast/http.hpp>
#include <boost/regex.hpp>

#include "namespaces.h"


namespace ouinet {

//------------------------------------------------------------------------------
// The different mechanisms an HTTP request can be routed over.
enum request_mechanism {
    // These mechanisms may be configured by the user.
    origin,      // send request to the origin HTTP server
    proxy,       // send request to proxy ouiservice
    injector,    // send request to injector ouiservice
    cache,       // retrieve resource from the cache

    // The following entries are for internal use only.
    _unknown,    // used e.g. in case of errors
    _front_end,  // handle the request internally
};


//------------------------------------------------------------------------------
// Request expressions can tell whether they match a given request
// (much like regular expressions match strings).
namespace reqexpr {
class ReqExpr;

// The type of functions that retrieve a given field (as a string) from a request.
typedef typename std::function<beast::string_view (const http::request<http::string_body>&)> field_getter;

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
// A request router holds the context and rules to decide the different mechanisms
// a request should be routed to until it finally succeeds,
// considering previous attempts.
class RequestRouter {
    public:
        virtual ~RequestRouter() { }

        // Decide which access mechanism to use for the given request.
        // If no more mechanisms can be attempted, return `request_mechanism::unknown`
        // and set the error code to `error::no_more_routes`.
        virtual enum request_mechanism get_next_mechanism(sys::error_code&) = 0;
};

// Route the provided request according to the given list of mechanisms.
std::unique_ptr<RequestRouter>
route( const http::request<http::string_body>& req
     , const std::vector<enum request_mechanism>& rmechs);

// Route the provided request according to the list of mechanisms associated
// with the first matching expression in the given list,
// otherwise route it according to the given list of default mechanisms.
std::unique_ptr<RequestRouter>
route( const http::request<http::string_body>& req
     , const std::vector<std::pair<const reqexpr::reqex&, const std::vector<enum request_mechanism>&>>& matches
     , const std::vector<enum request_mechanism>& def_rmechs );

} // ouinet namespace
