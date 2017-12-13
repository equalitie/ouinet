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
SimpleRequestRouter::get_next_mechanism(sys::error_code& ec)
{
    ec = sys::error_code();

    // Check whether possible routing mechanisms have been exhausted.
    if (req_mech == std::end(req_mechs)) {
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

    // Use the following configured mechanism and prepare for the next one.
    return *(req_mech++);
}


namespace ouinet {

//------------------------------------------------------------------------------
std::unique_ptr<RequestRouter>
route( const http::request<http::string_body>& req
     , const std::vector<enum request_mechanism>& rmechs)
{
    return std::make_unique<SimpleRequestRouter>(req, rmechs);
}

//------------------------------------------------------------------------------
std::unique_ptr<RequestRouter>
route( const http::request<http::string_body>& req
     , const std::vector<std::pair<const ReqExpr&, const std::vector<enum request_mechanism>&>>& matches
     , const std::vector<enum request_mechanism>& def_rmechs )
{
    // Delegate to a simple router
    // with the mechanisms associated with the matching expression (if any),
    // or with `def_rmechs` if none does.
    for (auto mit = matches.begin(); mit != matches.end(); ++mit)
        if (mit->first.match(req))
            return std::make_unique<SimpleRequestRouter>(req, mit->second);
    return std::make_unique<SimpleRequestRouter>(req, def_rmechs);
}

} // ouinet namespace
