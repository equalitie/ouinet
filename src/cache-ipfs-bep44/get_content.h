#pragma once

#include "cached_content.h"
#include "../or_throw.h"
#include "../bittorrent/dht.h"
#include "../util/bytes.h"

namespace ouinet {

inline
CachedContent get_content(asio_ipfs::node* ipfs_node, bittorrent::MainlineDht* dht, util::Ed25519PublicKey cache_public_key, std::string url, asio::yield_context yield)
{
    sys::error_code ec;

    std::string url_hash = util::bytes::to_string(util::sha1(url));
    boost::optional<bittorrent::MutableDataItem> item = dht->mutable_get(
        cache_public_key,
        url_hash,
        yield[ec]
    );

    if (ec) {
        return or_throw<CachedContent>(yield, ec);
    }

    if (!item) {
        return or_throw<CachedContent>(yield, asio::error::not_found);
    }

    if (!item->verify()) {
        return or_throw<CachedContent>(yield, asio::error::not_found);
    }

    boost::optional<std::string> content_hash = item->value.as_string();
    if (!content_hash) {
        return or_throw<CachedContent>(yield, asio::error::not_found);
    }

    std::string s = ipfs_node->cat(*content_hash, yield[ec]);
    boost::posix_time::ptime unix_epoch(boost::gregorian::date(1970, 1, 1));
    boost::posix_time::ptime ts = unix_epoch + boost::posix_time::milliseconds(item->sequence_number);

    return or_throw(yield, ec, CachedContent{ts, move(s)});
}

} // namespace
