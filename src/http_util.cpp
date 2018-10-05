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
    if (hp.empty() && req.version() == 10) {
        // HTTP/1.0 proxy client with no ``Host:``, use URI.
        network::uri uri(target.to_string());
        return make_pair( uri.host().to_string()
                        , uri.has_port() ? uri.port().to_string() : defport);
    }

    auto pos = hp.find(':');

    if (pos == string::npos) {
        return make_pair(hp.to_string(), defport);
    }

    return make_pair( hp.substr(0, pos).to_string()
                    , hp.substr(pos + 1).to_string());
}
