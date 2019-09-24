#pragma once

#include <boost/utility/string_view.hpp>
#include <boost/optional.hpp>
#include "number.h"

namespace ouinet { namespace parse {

template<class Proto /* one of asio::ip::{tcp,udp} */>
inline
typename Proto::endpoint
endpoint(boost::string_view s, sys::error_code& ec)
{
    using namespace std;
    auto pos = s.rfind(':');

    ec = sys::error_code();

    if (pos == string::npos) {
        ec = asio::error::invalid_argument;
        return {};
    }

    auto addr = asio::ip::address::from_string(s.substr(0, pos).to_string(), ec);

    if (ec) return {};

    boost::string_view port_sv = s.substr(pos+1);

    auto opt_port = parse::number<uint16_t>(port_sv);

    if (!opt_port) {
        ec = asio::error::invalid_argument;
        return {};
    }

    return {move(addr), *opt_port};
}

template<class Proto /* one of asio::ip::{tcp,udp} */>
inline
boost::optional<typename Proto::endpoint>
endpoint(boost::string_view s)
{
    sys::error_code ec;
    auto retval = endpoint<Proto>(s, ec);
    if (ec) return boost::none;
    return retval;
}

}} // namespaces
