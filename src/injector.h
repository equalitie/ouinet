#include <boost/beast/core.hpp>

#include "cache/http_sign.h"
#include "namespaces.h"
#include "util.h"
#include "http_util.h"
#include "http_logger.h"
#include "util/yield.h"

using namespace std;
using namespace ouinet;
using Request = http::request<http::string_body>;
using TcpLookup = asio::ip::tcp::resolver::results_type;

static bool allow_private_targets = false;

//------------------------------------------------------------------------------
// Resolve request target address, check whether it is valid
// and return lookup results.
// If not valid, set error code
// (the returned lookup may not be usable then).
static
TcpLookup
resolve_target(const Request& req
              , AsioExecutor exec
              , Cancel& cancel
              , Yield yield)
{
    TcpLookup lookup;
    sys::error_code ec;

    string host, port;
    tie(host, port) = util::get_host_port(req);

    // First test trivial cases (like "localhost" or "127.1.2.3").
    bool local = boost::regex_match(host, util::localhost_rx);
    bool priv = boost::regex_match(host, util::private_rx);

    // Resolve address and also use result for more sophisticaded checking.
    if (!local && (!priv || allow_private_targets)) {}
        lookup = util::tcp_async_resolve(host, port
                                         , exec
                                         , cancel
                                         , static_cast<asio::yield_context>(yield[ec]));

    if (ec) return or_throw<TcpLookup>(yield, ec);

    // Test non-trivial cases (like "[0::1]" or FQDNs pointing to loopback).
    for (auto r : lookup)
    {
        if ((local = boost::regex_match(r.endpoint().address().to_string()
                                        , util::localhost_rx)))
            break;
        if ((priv = boost::regex_match(r.endpoint().address().to_string()
                                      , util::private_rx)))
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

