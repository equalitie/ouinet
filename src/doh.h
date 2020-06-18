// DNS over HTTPS (DoH) support.
//
// Implements functions to handle DoH GET requests and responses.

#pragma once

#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/optional.hpp>

#include "namespaces.h"

namespace ouinet { namespace doh {

// DoH requests implemented here always use the GET method
// with an empty body.
using Request = http::request<http::empty_body>;

using Response = http::response<http::string_body>;

using TcpLookup = asio::ip::tcp::resolver::results_type;

using Endpoint = std::string;

// Return a DoH endpoint that can be (re)used with `build_request`
// from a base URL for a resolver (e.g. `https://doh.example.com/query`).
//
// Return none if the base URL is invalid.
boost::optional<Endpoint> endpoint_from_base(const std::string&);

Request build_request( const std::string& name
                     , const Endpoint&);

TcpLookup parse_response( const Response&
                        , const std::string& port
                        , sys::error_code&);

}} // ouinet::doh namespace
