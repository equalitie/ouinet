#include "request_routing.h"
#include "error.h"

using namespace ouinet;

using Request = http::request<http::string_body>;

//------------------------------------------------------------------------------
static bool is_front_end_request(const Request& req)
{
    auto host = req["Host"].to_string();

    if (host.substr(0, sizeof("localhost")) != "localhost") {
        return false;
    }

    return true;
}

//------------------------------------------------------------------------------
enum request_mechanism
ouinet::route_request( const Request& req
                     , RoutingContext& rctx
                     , sys::error_code& ec)
{
    ec = sys::error_code();

    // Check whether possible routing mechanisms have been exhausted.
    if (!rctx.more_req_mechs()) {
        ec = error::make_error_code(error::no_more_routes);
        return request_mechanism::_unknown;
    }

    // Send front-end requests to the front end
    if (is_front_end_request(req)) {
        return request_mechanism::_front_end;
    }

    // Send non-safe HTTP method requests to the origin server
    if (req.method() != http::verb::get && req.method() != http::verb::head) {
        return request_mechanism::origin;
    }

    // Use the following configured mechanism and proceed to the next one.
    return *(rctx.next_req_mech++);
}
