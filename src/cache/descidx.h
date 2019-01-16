// Utility functions to handle storing descriptors in indexes.

#pragma once

#include <iostream>
#include <string>

#include "../namespaces.h"
#include "../or_throw.h"
#include "../util.h"
#include "index.h"


namespace ouinet {

namespace descriptor {

static const std::string ipfs_prefix = "/ipfs/";
static const std::string zlib_prefix = "/zlib/";

// Get the serialized descriptor pointed to by an entry
// in the given `index` under the given `key`.
// The descriptor has been saved in the given stores (`ipfs_load`).
template <class LoadFunc>
inline
std::string get_from_index( const std::string& key
                          , ClientIndex& index
                          , LoadFunc ipfs_load
                          , Cancel& cancel
                          , asio::yield_context yield)
{
    using namespace std;

    sys::error_code ec;

    string desc_data = index.find(key, cancel, yield[ec]);

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
        desc_str = ipfs_load(desc_ipfs, cancel, yield[ec]);
    } else {
        cerr << "WARNING: Invalid index entry for descriptor of key: " << key << endl;
        ec = asio::error::not_found;
    }

    return or_throw(yield, ec, move(desc_str));
}

// Add an entry for the serialized descriptor `desc_data`
// in the given `index` under the given `key`.
// The descriptor is to be saved in the given stores (`ipfs_store`).
//
// Returns the result of `ipfs_store` and
// index-specific data to help reinsert the key->descriptor mapping.
template <class StoreFunc>
inline
std::pair<std::string, std::string>
put_into_index( const std::string& key, const std::string& desc_data
              , InjectorIndex& index
              , StoreFunc ipfs_store
              , asio::yield_context yield)
{
    using dcid_insd = std::pair<std::string, std::string>;
    sys::error_code ec;

    // Always store the descriptor itself in IPFS.
    std::string desc_ipfs = ipfs_store(desc_data, yield[ec]);
    if (ec)
        return or_throw<dcid_insd>(yield, ec);

    std::string ins_data;
    // Insert descriptor inline (if possible).
    ins_data = index.insert( key, zlib_prefix + util::zlib_compress(desc_data)
                           , yield[ec]);
    if (ec && ec != asio::error::message_size)
        return or_throw<dcid_insd>(yield, ec);

    // Insert IPFS link to descriptor.
    if (ec == asio::error::message_size) {
        ec = sys::error_code();
        ins_data = index.insert(key, ipfs_prefix + desc_ipfs, yield[ec]);
    }

    return or_throw(yield, ec, dcid_insd(move(desc_ipfs), move(ins_data)));
}

} // namespace descriptor
} // namespace ouinet
