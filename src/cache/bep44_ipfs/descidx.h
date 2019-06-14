// Utility functions to handle storing descriptors in indexes.

#pragma once

#include <iostream>
#include <string>
#include <boost/utility/string_view.hpp>

#include "../../namespaces.h"
#include "../../or_throw.h"
#include "../../util.h"
#include "bep44_index.h"


namespace ouinet { namespace bep44_ipfs { namespace descriptor {

static const std::string ipfs_prefix = "/ipfs/";
static const std::string zlib_prefix = "/zlib/";

// Get the serialized descriptor pointed to by an `desc_path`.  `desc_path` can
// be either "/zlib/<data>" or "/ipfs/<QmCID>".  In the latter case there will
// be one additional IO call to retrieve the descriptor from IPFS.
template <class LoadFunc>
inline
std::string from_path( boost::string_view desc_path
                     , LoadFunc ipfs_load
                     , Cancel& cancel
                     , asio::yield_context yield)
{
    using namespace std;

    sys::error_code ec;

    string desc_str;

    if (desc_path.substr(0, zlib_prefix.size()) == zlib_prefix) {
        // Retrieve descriptor from inline zlib-compressed data.
        string desc_zlib(desc_path.substr(zlib_prefix.length()));
        desc_str = util::zlib_decompress(desc_zlib, ec);
    } else if (desc_path.substr(0, ipfs_prefix.size()) == ipfs_prefix) {
        // Retrieve descriptor from IPFS link.
        string desc_ipfs(desc_path.substr(ipfs_prefix.length()));
        desc_str = ipfs_load(desc_ipfs, cancel, yield[ec]);
        assert(!cancel || ec == asio::error::operation_aborted);
    } else {
        ec = asio::error::not_found;
    }

    return or_throw(yield, ec, move(desc_str));
}

// Add an entry for the serialized descriptor `desc_data`
// in the given `index` under the given `key`.
// The descriptor is to be saved in the given stores (`ipfs_store`).
//
// Returns the result of `ipfs_store`,
// index-specific data to help reinsert the key->descriptor mapping,
// and whether insertion data links to the descriptor instead of embedding it.
template <class StoreFunc>
inline
std::tuple<std::string, std::string, bool>
put_into_index( const std::string& key, const std::string& desc_data
              , Bep44InjectorIndex& index
              , StoreFunc ipfs_store
              , bool perform_io
              , asio::yield_context yield)
{
    using dcid_insd_link = std::tuple<std::string, std::string, bool>;
    sys::error_code ec;

    // Always store the descriptor itself in IPFS.
    std::string desc_ipfs = ipfs_store(desc_data, yield[ec]);
    if (ec)
        return or_throw<dcid_insd_link>(yield, ec);

    std::string ins_data;
    std::string zvalue = zlib_prefix + util::zlib_compress(desc_data);

    if (perform_io) {
        // Insert descriptor inline (if possible).
        ins_data = index.insert(key, std::move(zvalue), yield[ec]);
    } else {
        ins_data = index.get_insert_message(key, std::move(zvalue), ec);
    }

    bool link_desc = ec == asio::error::message_size;
    if (ec && !link_desc)
        return or_throw<dcid_insd_link>(yield, ec);

    // Insert IPFS link to descriptor.
    if (link_desc) {
        ec = sys::error_code();

        auto value = ipfs_prefix + desc_ipfs;

        if (perform_io) {
            ins_data = index.insert(key, std::move(value), yield[ec]);
        } else {
            ins_data = index.get_insert_message(key, std::move(value), ec);
        }
    }

    return or_throw(yield, ec, dcid_insd_link(move(desc_ipfs), move(ins_data), link_desc));
}

} // namespace descriptor
} // namespace bep44_ipfs
} // namespace ouinet
