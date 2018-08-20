// Temporary, simplified URI descriptor format for a single HTTP response.
#pragma once

#include <boost/format.hpp>
#include <json.hpp>

#include "../namespaces.h"
#include "../or_throw.h"

namespace ouinet {

namespace descriptor {

template<class Cache>
inline
http::response<http::dynamic_body>
http_parse_json( Cache& cache, const std::string& raw_json
               , asio::yield_context yield) {

    using Response = http::response<http::dynamic_body>;

    sys::error_code ec;
    std::string url, head, body_link, body;

    // Parse the JSON HTTP descriptor, extract useful info.
    try {
        auto json = nlohmann::json::parse(raw_json);
        url = json["url"];
        head = json["head"];
        body_link = json["body_link"];
    } catch (const std::exception& e) {
        std::cerr << "WARNING: Malformed or invalid HTTP descriptor: " << e.what() << std::endl;
        std::cerr << "----------------" << std::endl;
        std::cerr << raw_json << std::endl;
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

    // - Add the response body.
    parser.put(asio::buffer(body), ec);
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
