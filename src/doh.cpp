#include "doh.h"

#include <boost/utility/string_view.hpp>

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

static
std::string
dns_query(const std::string& name)
{
    // TODO: implement
    return "TEST";
}

Request
build_request( const std::string& name
             , const Endpoint& ep)
{
    // DoH uses unpadded base64url as defined in RFC4648#5 (RFC8484#6).
    auto dq_b64 = util::base64_encode(dns_query(name));
    for (auto& c : dq_b64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    auto target_padded = ep + dq_b64;
    boost::string_view target(target_padded);
    while (target.ends_with('=')) target.remove_suffix(1);

    // TODO: keep host in endpoint
    auto ep_hstart = ep.find("://") + 3;
    auto ep_hend = ep.find('/', ep_hstart);
    boost::string_view ep_host(ep);
    ep_host.remove_suffix(ep.size() - ep_hend);
    ep_host.remove_prefix(ep_hstart);

    Request rq{http::verb::get, target, 11 /* HTTP/1.1 */};
    rq.set(http::field::host, ep_host);
    return rq;
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
