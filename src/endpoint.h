#pragma once

namespace ouinet {

struct GnunetEndpoint {
    std::string host;
    std::string port;
};

using Endpoint = boost::variant< asio::ip::tcp::endpoint
                               , GnunetEndpoint >;

inline
boost::optional<Endpoint> parse_endpoint(beast::string_view endpoint)
{
    using std::string;
    using beast::string_view;
    using asio::ip::tcp;

    auto as_tcp_endpoint = []( string_view host
                             , string_view port
                             ) -> Result<tcp::endpoint> {
        sys::error_code ec;
        auto ip = asio::ip::address::from_string(host.to_string(), ec);
        if (ec) return ec;
        return tcp::endpoint(ip, strtol(port.data(), 0, 10));
    };

    sys::error_code ec;

    string_view host;
    string_view port;

    std::tie(host, port) = util::split_host_port(endpoint);

    if (auto ep = as_tcp_endpoint(host, port)) {
        return Endpoint{*ep};
    }
    else {
        return Endpoint{GnunetEndpoint{host.to_string(), port.to_string()}};
    }

    return boost::none;
}

inline
bool is_gnunet_endpoint(const Endpoint& ep) {
    return boost::get<GnunetEndpoint>(&ep) ? true : false;
}

} // ouinet namespace
