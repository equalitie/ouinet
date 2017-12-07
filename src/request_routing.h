#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <boost/beast/core/string.hpp>
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

// XXXX
class RequestMatch {
    public:
        virtual ~RequestMatch() { }

        virtual bool match(const http::request<http::string_body>&) const = 0;
};

class RegexRequestMatch : public RequestMatch {
    public:
        typedef typename std::function<beast::string_view (const http::request<http::string_body>&)> field_getter;
        static field_getter target_getter() {
            return [](const http::request<http::string_body>& r) {return r.target();};
        }
        static field_getter header_getter(const std::string& h) {
            return [=](const http::request<http::string_body>& r) {return r[h];};  // TODO check capture mode
        }

    private:
        const field_getter& get_field;
        const boost::regex regexp;

    public:
        RegexRequestMatch( const field_getter& gf
                         , const boost::regex& rx)
            : get_field(gf), regexp(rx) { };

        bool match(const http::request<http::string_body>& req) const {
            return boost::regex_match(get_field(req).to_string(), regexp);
        }

        RegexRequestMatch( const field_getter& gf
                         , const std::string& rx)
            : RegexRequestMatch(gf, boost::regex(rx)) { };
};

// TODO class TrueRequestMatch()  // for defaults
// TODO class AllRequestMatch(vector<RequestMatch>)

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

// Route the provided request according to the list of mechanisms associated
// with the first successful match in the given list,
// otherwise route it according to the given list of default mechanisms.
class MultiMatchRequestRouter : public RequestRouter {
    private:
        std::unique_ptr<SimpleRequestRouter> rr;  // delegate to this

    public:
        MultiMatchRequestRouter( const http::request<http::string_body>& req
                               , const std::vector<std::pair<const RequestMatch&, const std::vector<enum request_mechanism>&>>& matches
                               , const std::vector<enum request_mechanism>& def_rmechs)
        {
            // Delegate to a simple router
            // with the mechanisms associated with the successful match (if any),
            // or with `def_rmechs` if none does.
            for (auto mit = matches.begin(); mit != matches.end(); ++mit) {
                if (mit->first.match(req)) {
                    rr = std::make_unique<SimpleRequestRouter>(req, mit->second);
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
