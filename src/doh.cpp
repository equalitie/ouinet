#include "doh.h"

#include <sstream>

#include <boost/utility/string_view.hpp>

#include "split_string.h"
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
    // The hardwired values here are taken from a capture of
    // Firefox DoH traffic.
    static const std::string dq_prefix = (
        // DNS message header
        "\x00\x00"  // ID set to 0 as per RFC8484#4.1
        "\x01\x00"  // query of type QUERY, recursive
        "\x00\x01"  // 1 question record
        "\x00\x00"  // 0 answer records
        "\x00\x00"  // 0 name server records
        "\x00\x01"  // 1 additional record (EDNS)
    );
    static const std::string dq_suffix = (
        // DNS question
        // (queried name comes here)
        "\x00\x01"  // A (IPv4) type  // TODO: IPv6? (28)
        "\x00\x01"  // IN (Internet) class
        // EDNS (RFC6891#6.1.2)
        // All stuff from here on seems to explicitly tell the server that
        // no source address bits are relevant for choosing
        // between different possible answers.
        // TODO: Consider dropping options entirely to minimize query string
        // and thus increase chances of sharing resulting DoH URL with others
        // (set additional records to 0 above if so).
        "\x00"      // root domain
        "\x00\x29"  // OPT (41)
        "\x10\x00"  // 4K payload size
        "\x00"      // unextended RCODE (RFC6891#6.1.3)
        "\x00"      // EDNS version 0 (RFC6891#6.1.3)
        "\x00\x00"  // DNSSEC not ok, zeros (RFC6891#6.1.4)
        "\x00\x08"  // RDATA length
        // EDNS RDATA
        // Actual EDNS option: client subnet (RFC7871#6)
        "\x00\x08"  // option code 8 (client subnet)
        "\x00\x04"  // option length
        "\x00\x01"  // family 1 (IPv4)  // TODO: IPv6? (2)
        "\x00"      // source prefix length
        "\x00"      // scope prefix-length, zero in queries
    );

    std::stringstream dq;

    dq << dq_prefix;

    // Turn "example.com" into "\x07example\x03com\x00" as per RC1035#3.1.
    // TODO: check name.size() < 254
    for (auto l : SplitString(name, '.'))
        // TODO: check 0 < l.size() < 64
        dq << static_cast<uint8_t>(l.size()) << l;
    dq << '\0';

    dq << dq_suffix;

    return dq.str();
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
