#pragma once

#include <boost/beast/http.hpp>

#include "namespaces.h"

namespace ouinet {

// The different mechanisms an HTTP request can be routed over.
enum request_mechanism {
    front_end,  // handle the request internally
    origin,     // send request to the origin HTTP server
    proxy,      // send request to proxy ouiservice
    injector,   // send request to injector ouiservice
    cache       // retrieve resource from the cache
};

enum request_mechanism
route_request(http::request<http::string_body>);

} // ouinet namespace
