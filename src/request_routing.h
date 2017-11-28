#pragma once

#include <boost/beast/http.hpp>

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

// These hard-wired access mechanisms are attempted in order for all normal requests.
const enum request_mechanism default_request_mechanisms[] = {
    request_mechanism::cache,
    request_mechanism::injector,
};

class RequestRouter {
    private:
        const http::request<http::string_body> req;
        const enum request_mechanism* req_mech;

    public:
        RequestRouter(const http::request<http::string_body>& r) : req(r), req_mech(std::begin(default_request_mechanisms)) { }

        // Decide which access mechanism to use for the given request,
        // given previous attempts.
        enum request_mechanism get_next_mechanism(sys::error_code&);
};

} // ouinet namespace
