// DNS over HTTPS (DoH) support.
//
// Implements functions to handle DoH GET requests and responses.

#pragma once

#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/string_body.hpp>

#include "namespaces.h"

namespace ouinet { namespace doh {

// DoH requests implemented here always use the GET method
// with an empty body.
using Request = http::request<http::empty_body>;

using Response = http::response<http::string_body>;

using TcpLookup = asio::ip::tcp::resolver::results_type;

Request build_request( const std::string& name
                     , const std::string& base);

TcpLookup parse_response( const Response&
                        , const std::string& port
                        , sys::error_code&);

}} // ouinet::doh namespace
