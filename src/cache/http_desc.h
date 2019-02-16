// Temporary, simplified URI descriptor format for a single HTTP response.
//
// See `doc/descriptor-*.json` for the target format.
#pragma once

#include <sstream>

#include <boost/format.hpp>
#include <boost/optional.hpp>
#include <json.hpp>

#include "cache_entry.h"
#include "../namespaces.h"
#include "../or_throw.h"
#include "../util.h"
#include "../util/signal.h"

namespace ouinet {

struct Descriptor {
    using ptime = boost::posix_time::ptime;

    static unsigned version() { return 0; }

    std::string url;
    std::string request_id;
    ptime       timestamp;
    std::string head;
    std::string body_link;

    std::string serialize() const {
        static const auto ts_to_str = [](ptime ts) {
            return boost::posix_time::to_iso_extended_string(ts) + 'Z';
        };

        return nlohmann::json { { "!ouinet_version", version() }
                              , { "url"            , url }
                              , { "id"             , request_id }
                              , { "ts"             , ts_to_str(timestamp) }
                              , { "head"           , head }
                              , { "body_link"      , body_link }
                              }
                              .dump();
    }

    static boost::optional<Descriptor> deserialize(std::string data) {
        try {
            auto json = nlohmann::json::parse(data);

            auto v = json["!ouinet_version"];

            if (!v.is_null() && unsigned(v) != version()) {
                return boost::none;
            }

            Descriptor dsc;

            dsc.url        = json["url"];
            dsc.request_id = json["id"];
            dsc.timestamp  = boost::posix_time::from_iso_extended_string(json["ts"]);
            dsc.head       = json["head"];
            dsc.body_link  = json["body_link"];

            return dsc;
        } catch (const std::exception& e) {
            return boost::none;
        }
    }
};

namespace descriptor {

// For the given HTTP request `rq` and response `rs`,
// seed body data using `ipfs_store`,
// then create an HTTP descriptor with the given `id` for the URL and response,
// and return it.
template <class StoreFunc>
static inline
std::string
http_create( const std::string& id
           , boost::posix_time::ptime ts
           , const http::request<http::string_body>& rq
           , const http::response<http::dynamic_body>& rs
           , StoreFunc ipfs_store
           , asio::yield_context yield) {

    using namespace std;

    sys::error_code ec;

    string ipfs_id = ipfs_store(
            beast::buffers_to_string(rs.body().data()), yield[ec]);

    if (ec) return or_throw<string>(yield, ec);

    stringstream rsh_ss;
    rsh_ss << rs.base();

    return Descriptor{ util::canonical_url(rq.target())
                     , id
                     , ts
                     , rsh_ss.str()
                     , ipfs_id
                     }.serialize();
}

// For the given HTTP descriptor serialized in `desc_data`,
// retrieve the head from the descriptor and the body data using `ipfs_load`,
// and return the descriptor identifier and HTTP response cache entry.
//
// TODO: Instead of the identifier,
// the parsed `Descriptor` itself should probably be returned,
// but the identifier suffices right now.
template <class LoadFunc>
static inline
std::pair<std::string, CacheEntry>
http_parse( const std::string& desc_data
          , LoadFunc ipfs_load
          , Cancel& cancel
          , asio::yield_context yield)
{
    using IdAndCE = std::pair<std::string, CacheEntry>;
    using Response = http::response<http::dynamic_body>;

    sys::error_code ec;

    boost::optional<Descriptor> dsc = Descriptor::deserialize(desc_data);

    if (!dsc) {
        std::cerr << "WARNING: Malformed or invalid HTTP descriptor: " << std::endl;
        std::cerr << "----------------" << std::endl;
        std::cerr << desc_data << std::endl;
        std::cerr << "----------------" << std::endl;
        ec = asio::error::invalid_argument;
    }

    if (ec) return or_throw<IdAndCE>(yield, ec);

    // Get the HTTP response body (stored independently).
    std::string body = ipfs_load(dsc->body_link, cancel, yield[ec]);

    // Build an HTTP response from the head in the descriptor and the retrieved body.
    http::response_parser<Response::body_type> parser;
    parser.eager(true);

    // - Parse the response head.
    parser.put(asio::buffer(dsc->head), ec);

    if (!ec && !parser.is_header_done()) {
        ec = asio::error::invalid_argument;
    }

    if (ec) {
        std::cerr << "WARNING: Malformed or incomplete HTTP head in descriptor" << std::endl;
        std::cerr << "----------------" << std::endl;
        std::cerr << dsc->head << std::endl;
        std::cerr << "----------------" << std::endl;
        return or_throw<IdAndCE>(yield, ec);
    }

    Response res = parser.release();
    Response::body_type::reader reader(res, res.body());
    reader.put(asio::buffer(body), ec);

    if (ec) {
        std::cerr << "WARNING: Failed to put body into the response "
            << ec.message() << std::endl;

        return or_throw<IdAndCE>(yield, asio::error::invalid_argument);
    }

    res.prepare_payload();

    return IdAndCE( dsc->request_id
                  , CacheEntry{dsc->timestamp, std::move(res)});
}

} // ouinet::descriptor namespace

} // ouinet namespace
