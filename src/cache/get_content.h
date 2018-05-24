#pragma once

#include "cached_content.h"
#include "../or_throw.h"

namespace ouinet {

template<class Db>
inline
CachedContent get_content(Db& db, std::string url, asio::yield_context yield)
{
    sys::error_code ec;

    std::string raw_json = db.query(url, yield[ec]);

    if (ec) {
        return or_throw<CachedContent>(yield, ec);
    }

    std::string content_hash;
    boost::posix_time::ptime ts;

    try {
        auto json = Json::parse(raw_json);

        ts           = boost::posix_time::from_iso_extended_string(json["ts"]);
        content_hash = json["value"];
    }
    catch(const std::exception& e) {
        std::cerr << "Problem parsing data from cache: " << e.what()
                  << std::endl << "  \"" << raw_json << "\""
                  << std::endl;

        ec = asio::error::not_found;
    }

    if (ec) {
        return or_throw<CachedContent>(yield, ec);
    }

    std::string s = db.ipfs_node().cat(content_hash, yield[ec]);

    return or_throw(yield, ec, CachedContent{ts, move(s)});
}

} // namespace
