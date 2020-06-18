#include "doh.h"
#include "util.h"

namespace ouinet { namespace doh {

boost::optional<Endpoint>
endpoint_from_base(const std::string& base)
{
    util::url_match um;
    if (!util::match_http_url(base, um) || !um.fragment.empty())
        return boost::none;
    um.query += um.query.empty() ? "dns=" : "&dns=";
    return um.reassemble();
}

Request
build_request( const std::string& name
             , const Endpoint& ep)
{
    // TODO: implement
    return Request{};
}

TcpLookup
parse_response( const Response& rs
              , const std::string& port
              , sys::error_code& ec)
{
    // TODO: implement
    ec = asio::error::operation_not_supported;
    return TcpLookup{};
}

}} // ouinet::doh namespace
