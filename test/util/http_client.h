#pragma once

#include "namespaces.h"

#include <expected>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/core/tcp_stream.hpp>

namespace ouinet {

namespace util { class Url; }
class Async;
class Client;

asio::ssl::stream<boost::beast::tcp_stream> setup_tls_stream(asio::ip::tcp::socket, asio::ssl::context&, std::string host);

std::expected<http::response<http::string_body>, sys::error_code>
fetch_from_origin(util::Url, asio::ssl::context&, Async);

std::expected<http::response<http::string_body>, sys::error_code>
fetch_from_origin(util::Url, Async);

} // namespace
