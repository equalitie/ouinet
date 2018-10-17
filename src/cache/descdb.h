// Utility functions to handle storing descriptors in data bases.

#pragma once

#include <iostream>
#include <string>

#include <asio_ipfs.h>

#include "../namespaces.h"
#include "../or_throw.h"
#include "../util.h"
#include "db.h"
#include "bep44_db.h"


namespace ouinet {

namespace descriptor {

static const std::string ipfs_prefix = "/ipfs/";
static const std::string zlib_prefix = "/zlib/";

// This is a decision we take here and not at the db level,
// since a db just stores a string
// and it does not differentiate between an inlined descriptor and a link to it.
// An alternative would be to always attempt to store the descriptor inlined
// and attempt again with a link in case of getting `asio::error::message_size`.
// However at the moment we do not want to even attempt inlining
// with the IPFS-based B-tree cache index.
inline
bool db_can_inline(InjectorDb& db) {
    return false;
}

inline
bool db_can_inline(Bep44InjectorDb& db) {
    return true;  // only attempt inlining with BEP44
}

inline
std::string get_from_db( const std::string& key
                       , ClientDb& db, asio_ipfs::node& ipfs
                       , asio::yield_context yield)
{
    using namespace std;

    sys::error_code ec;

    string desc_data = db.find(key, yield[ec]);

    if (ec)
        return or_throw<string>(yield, ec);

    string desc_str;
    if (desc_data.find(zlib_prefix) == 0) {
        // Retrieve descriptor from inline zlib-compressed data.
        string desc_zlib(move(desc_data.substr(zlib_prefix.length())));
        desc_str = util::zlib_decompress(desc_zlib, ec);
    } else if (desc_data.find(ipfs_prefix) == 0) {
        // Retrieve descriptor from IPFS link.
        string desc_ipfs(move(desc_data.substr(ipfs_prefix.length())));
        desc_str = ipfs.cat(desc_ipfs, yield[ec]);
    } else {
        cerr << "WARNING: Invalid index entry for descriptor of key: " << key << endl;
        ec = asio::error::not_found;
    }

    return or_throw(yield, ec, move(desc_str));
}

inline
void put_into_db( const std::string& key
                , const std::string& desc_data
                , const std::string& desc_ipfs
                , InjectorDb& db
                , asio::yield_context yield)
{
    sys::error_code ec;

    // Insert descriptor inline (if possible).
    bool can_inline = db_can_inline(db);
    if (can_inline) {
        auto compressed_desc = util::zlib_compress(desc_data);
        db.insert(key, zlib_prefix + compressed_desc, yield[ec]);
    }
    // Insert IPFS link to descriptor.
    if (!can_inline || ec == asio::error::message_size) {
        db.insert(key, ipfs_prefix + desc_ipfs, yield[ec]);
    }

    return or_throw(yield, ec);
}

} // namespace descriptor
} // namespace ouinet
