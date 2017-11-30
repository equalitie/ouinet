#pragma once

#include <vector>

#include <boost/beast/http.hpp>
#include <boost/regex.hpp>

#include "namespaces.h"

namespace ouinet {

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

// Holds the context and rules to decide the different mechanisms
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
class SimpleRequestRouter : public RequestRouter {
    private:
        const http::request<http::string_body> req;
        const std::vector<enum request_mechanism>& req_mechs;
        std::vector<enum request_mechanism>::const_iterator req_mech;

    public:
        SimpleRequestRouter( const http::request<http::string_body>& r
                            , const std::vector<enum request_mechanism>& rmechs)
            : req(r), req_mechs(rmechs), req_mech(std::begin(req_mechs)) { }

        enum request_mechanism get_next_mechanism(sys::error_code&) override;
};

// Route the provided request according to the given list of match mechanisms
// if the request target matches one of the given regular expressions,
// otherwise route it according to the given list of default mechanisms.
class MatchTargetRequestRouter : public RequestRouter {
    private:
        std::unique_ptr<SimpleRequestRouter> rr;  // delegate to this

    public:
        MatchTargetRequestRouter( const http::request<http::string_body>& req
                                , const std::vector<boost::regex>& target_rxs
                                , const std::vector<enum request_mechanism>& match_rmechs
                                , const std::vector<enum request_mechanism>& def_rmechs)
        {
            // Delegate to a simple router
            // with `match_rmechs` if the target matches any of the given regexes,
            // or with `def_rmechs` if it does not.
            auto target = req.target().to_string();
            for (auto rxit = target_rxs.begin(); rxit != target_rxs.end(); ++rxit) {
                if (boost::regex_match(target, *rxit)) {
                    rr = std::make_unique<SimpleRequestRouter>(req, match_rmechs);
                    return;
                }
            }
            rr = std::make_unique<SimpleRequestRouter>(req, def_rmechs);
        }

        enum request_mechanism get_next_mechanism(sys::error_code& ec) override
        {
            return rr->get_next_mechanism(ec);
        }
};

} // ouinet namespace
