#pragma once

#include "../parse/number.h"
#include "address.h"
#include "yield.h"

#include "cxx/dns.h"

namespace ouinet::util
{
    using tcp = asio::ip::tcp;
    using TcpLookup = tcp::resolver::results_type;
    using Answers = std::vector<asio::ip::address>;

    inline
    TcpLookup resolve( const std::string& host
                     , const std::string& port
                     , bool do_doh
                     , Cancel& cancel
                     , YieldContext yield)
    {
        using TcpEndpoint = typename TcpLookup::endpoint_type;

        boost::string_view portsv(port);
        auto portn_o = parse::number<unsigned short>(portsv);
        if (!portn_o) return or_throw<TcpLookup>(yield, asio::error::invalid_argument);

        // Build and return lookup if `host` is already a network address.
        {
            sys::error_code e;
            auto addr = asio::ip::make_address(host, e);
            if (!e) return TcpLookup::create(TcpEndpoint{std::move(addr), *portn_o}, host, port);
        }

        sys::error_code ec;
        dns::Resolver resolver{do_doh};
        const auto answers46 = yield[ec].tag("resolve host").run([&](auto y)
        {
            return resolver.resolve(host, y[ec]);
        });
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<TcpLookup>(yield, ec);

        const AddrsAsEndpoints<Answers, TcpEndpoint> eps{answers46, *portn_o};
        return TcpLookup::create(eps.begin(), eps.end(), host, port);
    }
}
