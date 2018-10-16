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
#include "../http_util.h"
#include "../bittorrent/dht.h"
#include "../util/scheduler.h"

using namespace std;
using namespace ouinet;
namespace bt = ouinet::bittorrent;

// TODO: Factor out descriptor cache encoding stuff,
// along with IPFS storage of descriptors in `http_desc.h`.
static const std::string desc_ipfs_prefix = "/ipfs/";

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

    // TODO: use string_view for key
    auto key = rq.target().to_string();

    // Insert IPFS link to descriptor.
    get_db(db_type)->insert(move(key), desc_ipfs_prefix + desc.first, yield[ec]);

    if (!ec && *wd) ec = asio::error::operation_aborted;

    return or_throw(yield, ec, move(desc.second));
}

string CacheInjector::get_descriptor( string url
                                    , DbType db_type
                                    , asio::yield_context yield)
{
    sys::error_code ec;

    string desc_data = get_db(db_type)->find(url, yield[ec]);

    // Retrieve descriptor from IPFS link.
    if (!ec && desc_data.find(desc_ipfs_prefix) != 0) {
        cerr << "WARNING: Invalid index entry for descriptor of " << url << endl;
        ec = asio::error::not_found;
    }

    if (ec)
        return or_throw<string>(yield, ec);

    string desc_ipfs(desc_data.substr(desc_ipfs_prefix.length()));
    return _ipfs_node->cat(desc_ipfs, yield);
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
