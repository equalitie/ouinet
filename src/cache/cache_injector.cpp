#include <boost/asio/io_service.hpp>
#include <assert.h>
#include <iostream>
#include <chrono>

#include "cache_injector.h"
#include "http_desc.h"

#include <asio_ipfs.h>
#include "btree_db.h"
#include "bep44_db.h"
#include "publisher.h"
#include "../util.h"
#include "../http_util.h"
#include "../bittorrent/dht.h"
#include "../util/scheduler.h"

using namespace std;
using namespace ouinet;
namespace bt = ouinet::bittorrent;

// TODO: Factor out descriptor cache encoding stuff,
// along with IPFS storage of descriptors in `http_desc.h`.
static const std::string desc_ipfs_prefix = "/ipfs/";
static const std::string desc_zlib_prefix = "/zlib/";

// This is a decision we take here and not at the db level,
// since a db just stores a string
// and it does not differentiate between an inlined descriptor and a link to it.
// An alternative would be to always attempt to store the descriptor inlined
// and attempt again with a link in case of getting `asio::error::message_size`.
// However at the moment we do not want to even attempt inlining
// with the IPFS-based B-tree cache index.
static
bool db_can_inline(DbType db_type) {
    return (db_type == DbType::bep44);  // only attempt inlining with BEP44
}

CacheInjector::CacheInjector
        ( asio::io_service& ios
        , util::Ed25519PrivateKey bt_privkey
        , fs::path path_to_repo)
    : _ipfs_node(new asio_ipfs::node(ios, (path_to_repo/"ipfs").native()))
    , _bt_dht(new bt::MainlineDht(ios))
    , _publisher(new Publisher(*_ipfs_node, *_bt_dht, bt_privkey))
    , _btree_db(new BTreeInjectorDb(*_ipfs_node, *_publisher, path_to_repo))
    , _scheduler(new Scheduler(ios, _concurrency))
    , _was_destroyed(make_shared<bool>(false))
{
    _bt_dht->set_interfaces({asio::ip::address_v4::any()});
    _bep44_db.reset(new Bep44InjectorDb(*_bt_dht, bt_privkey));
}

string CacheInjector::ipfs_id() const
{
    return _ipfs_node->id();
}

InjectorDb* CacheInjector::get_db(DbType db_type) const
{
    switch (db_type) {
        case DbType::btree: return _btree_db.get();
        case DbType::bep44: return _bep44_db.get();
    }

    return nullptr;
}

void CacheInjector::put_descriptor( string url
                                  , const string& desc_data
                                  , const string& desc_ipfs
                                  , DbType db_type
                                  , asio::yield_context yield)
{
    sys::error_code ec;

    // Insert descriptor inline (if possible).
    bool can_inline = db_can_inline(db_type);
    if (can_inline) {
        auto compressed_desc = util::zlib_compress(desc_data);
        get_db(db_type)->insert(url, desc_zlib_prefix + compressed_desc, yield[ec]);
    }
    // Insert IPFS link to descriptor.
    if (!can_inline || ec == asio::error::message_size) {
        get_db(db_type)->insert(url, desc_ipfs_prefix + desc_ipfs, yield[ec]);
    }

    return or_throw(yield, ec);
}

// TODO: Factor out descriptor cache encoding stuff,
// along with IPFS storage of descriptors in `http_desc.h`.
string CacheInjector::insert_content( Request rq
                                    , Response rs
                                    , DbType db_type
                                    , asio::yield_context yield)
{
    auto wd = _was_destroyed;

    sys::error_code ec;

    auto id = rs[http_::response_injection_id_hdr].to_string();
    rs.erase(http_::response_injection_id_hdr);

    auto ts = boost::posix_time::microsec_clock::universal_time();

    pair<string, string> desc;

    {
        auto slot = _scheduler->wait_for_slot(yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);

        desc = descriptor::http_create(*_ipfs_node, id, ts, rq, rs, yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);
    }

    put_descriptor( rq.target().to_string(), desc.second, desc.first
                  , db_type, yield[ec]);

    if (!ec && *wd) ec = asio::error::operation_aborted;

    return or_throw(yield, ec, move(desc.second));
}

string CacheInjector::get_descriptor( string url
                                    , DbType db_type
                                    , asio::yield_context yield)
{
    sys::error_code ec;

    string desc_data = get_db(db_type)->find(url, yield[ec]);

    if (ec)
        return or_throw<string>(yield, ec);

    string desc_str;
    if (desc_data.find(desc_zlib_prefix) == 0) {
        // Retrieve descriptor from inline zlib-compressed data.
        string desc_zlib(move(desc_data.substr(desc_zlib_prefix.length())));
        desc_str = util::zlib_decompress(desc_zlib, ec);
    } else if (desc_data.find(desc_ipfs_prefix) == 0) {
        // Retrieve descriptor from IPFS link.
        string desc_ipfs(move(desc_data.substr(desc_ipfs_prefix.length())));
        desc_str = _ipfs_node->cat(desc_ipfs, yield[ec]);
    } else {
        cerr << "WARNING: Invalid index entry for descriptor of " << url << endl;
        ec = asio::error::not_found;
    }

    return or_throw(yield, ec, move(desc_str));
}

CacheEntry CacheInjector::get_content( string url
                                     , DbType db_type
                                     , asio::yield_context yield)
{
    sys::error_code ec;

    string desc_data = get_descriptor(url, db_type, yield[ec]);

    if (ec) return or_throw<CacheEntry>(yield, ec);

    return descriptor::http_parse(*_ipfs_node, desc_data, yield);
}

CacheInjector::~CacheInjector()
{
    *_was_destroyed = true;
}
