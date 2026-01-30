#include "dns.h"
#include "http_util.h"
#include "util/dns.h"

namespace ouinet::util {

using namespace std;

// Resolve request target address, check whether it is valid
// and return lookup results.
// If not valid, set error code
// (the returned lookup may not be usable then).
TcpLookup
resolve_target(const http::request_header<>& req
              , bool allow_private_targets
              , bool do_doh
              , AsioExecutor exec
              , Cancel& cancel
              , YieldContext yield)
{
    TcpLookup lookup;
    sys::error_code ec;

    string host, port;
    tie(host, port) = util::get_host_port(req);

    // First test trivial cases (like "localhost" or "127.1.2.3").
    bool local = boost::regex_match(host, util::localhost_rx);
    bool priv = boost::regex_match(host, util::private_addr_rx);

    // Resolve address and also use result for more sophisticaded checking.
    if (!local && (!priv || allow_private_targets))
    {
        lookup = do_doh
               ? util::resolve_tcp_doh( host, port, cancel, yield[ec] )
               : util::resolve_tcp_async( host, port
                                        , exec
                                        , cancel
                                        , yield[ec].native());
    }

    if (ec) return or_throw<TcpLookup>(yield, ec);

    // Test non-trivial cases (like "[0::1]" or FQDNs pointing to loopback).
    for (auto r : lookup)
    {
        if ((local = boost::regex_match(r.endpoint().address().to_string()
                                        , util::localhost_rx)))
            break;
        if ((priv = boost::regex_match(r.endpoint().address().to_string()
                                      , util::private_addr_rx)))
            if (!allow_private_targets)
                break;
    }

    if (local || (priv && !allow_private_targets))
    {
        ec = asio::error::invalid_argument;
        return or_throw<TcpLookup>(yield, ec);
    }

    return or_throw(yield, ec, move(lookup));
}

} // namespace
