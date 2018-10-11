#include "http_util.h"

#include <boost/asio/error.hpp>
#include <network/uri.hpp>


using namespace std;


pair<string, string>
ouinet::util::get_host_port(const http::request<http::string_body>& req)
{
    auto target = req.target();
    auto defport = target.starts_with("https:") ? "443" : "80";

    auto hp = req[http::field::host];
    auto cpos = hp.rfind(':');
    string host, port;

    if (hp.empty() && req.version() == 10) {
        // HTTP/1.0 proxy client with no ``Host:``, use URI.
        network::uri uri(target.to_string());
        host = uri.host().to_string();
        port = uri.has_port() ? uri.port().to_string() : defport;
    } else if (cpos == string::npos || hp[hp.length() - 1] == ']') {
        // ``Host:`` header present, no port.
        host = hp.to_string();
        port = defport;
    } else {
        // ``Host:`` header present, explicit port.
        host = hp.substr(0, cpos).to_string();
        port = hp.substr(cpos + 1).to_string();
    }

    // Remove brackets from IPv6 hosts.
    if (host[0] == '[')
        host = host.substr(1, host.length() - 2);

    return make_pair(host, port);
}
