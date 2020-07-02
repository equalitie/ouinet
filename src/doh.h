// DNS over HTTPS (DoH) support.
//
// Implements functions to handle DoH GET requests and responses.

#pragma once

#include <string>
#include <vector>

#include <boost/asio/ip/address.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/optional.hpp>

#include "namespaces.h"

namespace ouinet { namespace doh {

// The maximum payload size expected in responses, as per RFC6891#6.2.3.
// It can be used as an upper boundary for the body size of `Response` below.
static const size_t payload_size = 4096;

// DoH requests implemented here always use the GET method
// with an empty body.
using Request = http::request<http::empty_body>;

using Response = http::response<http::string_body>;

using Answers = std::vector<asio::ip::address>;

using Endpoint = std::string;

// Return a DoH endpoint that can be (re)used with `build_request`
// from a base URL for a resolver (e.g. `https://doh.example.com/query`).
//
// Return none if the base URL is invalid.
boost::optional<Endpoint> endpoint_from_base(const std::string&);

// Return a DoH request for IPv4 (type A) addresses of the given `name`,
// to be sent to the given DoH `endpoint`.
//
// Return none if the name is invalid.
boost::optional<Request> build_request_ipv4( const std::string& name
                                           , const Endpoint&);

// Return the addresses in the answers for the given host
// in the given response.
//
// Irrelevant answers in the response are discarded.
Answers parse_response( const Response&
                      , const std::string& host
                      , sys::error_code&);

}} // ouinet::doh namespace
