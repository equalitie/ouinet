// Temporary, simplified URI descriptor format for a single HTTP response.
//
// See `doc/descriptor-*.json` for the target format.
#pragma once

#include <sstream>

#include <boost/format.hpp>
#include <json.hpp>

#include "../namespaces.h"
#include "../or_throw.h"

namespace ouinet {

namespace descriptor {

// For the given HTTP request `rq` and response `rs`, seed body data to the `cache`,
// then create an HTTP descriptor for the URL and response,
// and pass it to the given callback.
template<class Cache>
inline
std::string
http_create( Cache& cache
           , const http::request<http::string_body>& rq
           , const http::response<http::dynamic_body>& rs
           , asio::yield_context yield) {

    // TODO: Do it more efficiently?
    sys::error_code ec;
    auto ipfs_id = cache.put_data( beast::buffers_to_string(rs.body().data())
                                 , yield[ec]);

    auto url = rq.target().to_string();
    if (ec) {
        std::cout << "!Data seeding failed: " << url
                  << " " << ec.message() << std::endl;
        return or_throw<std::string>(yield, ec);
    }

    // Create the descriptor.
    // TODO: This is a *temporary format* with the bare minimum to test
    // head/body splitting of HTTP responses.
    std::stringstream rsh_ss;
    rsh_ss << rs.base();

    nlohmann::json desc;
    desc["url"] = url;
    desc["head"] = rsh_ss.str();
    desc["body_link"] = ipfs_id;

    return std::move(desc.dump());
}

// For the given HTTP descriptor serialized in `desc_data`,
// retrieve the head from the descriptor and the body data from the `cache`,
// assemble and return the HTTP response.
template<class Cache>
inline
http::response<http::dynamic_body>
http_parse( Cache& cache, const std::string& desc_data
          , asio::yield_context yield) {

    using Response = http::response<http::dynamic_body>;

    sys::error_code ec;
    std::string url, head, body_link, body;

    // Parse the JSON HTTP descriptor, extract useful info.
    try {
        auto json = nlohmann::json::parse(desc_data);
        url = json["url"];
        head = json["head"];
        body_link = json["body_link"];
    } catch (const std::exception& e) {
        std::cerr << "WARNING: Malformed or invalid HTTP descriptor: " << e.what() << std::endl;
        std::cerr << "----------------" << std::endl;
        std::cerr << desc_data << std::endl;
        std::cerr << "----------------" << std::endl;
        ec = asio::error::invalid_argument;  // though ``bad_descriptor`` would rock
    }

    if (!ec)
        // Get the HTTP response body (stored independently).
        body = cache.get_data(body_link, yield[ec]);

    if (ec)
        return or_throw<Response>(yield, ec);

    // Build an HTTP response from the head in the descriptor and the retrieved body.
    http::response_parser<Response::body_type> parser;
    parser.eager(true);

    // - Parse the response head.
    parser.put(asio::buffer(head), ec);
    if (ec || !parser.is_header_done()) {
        std::cerr << "WARNING: Malformed or incomplete HTTP head in descriptor" << std::endl;
        std::cerr << "----------------" << std::endl;
        std::cerr << head << std::endl;
        std::cerr << "----------------" << std::endl;
        ec = asio::error::invalid_argument;
        return or_throw<Response>(yield, ec);
    }

    // - Add the response body (if needed).
    if (body.length() > 0)
        parser.put(asio::buffer(body), ec);
    else
        parser.put_eof(ec);
    if (ec || !parser.is_done()) {
        std::cerr
          << (boost::format
              ("WARNING: Incomplete HTTP body in cache (%1% out of %2% bytes) for %3%")
              % body.length() % parser.get()[http::field::content_length] % url)
          << std::endl;
        ec = asio::error::invalid_argument;
        return or_throw<Response>(yield, ec);
    }

    return parser.release();
}

} // ouinet::descriptor namespace

} // ouinet namespace
