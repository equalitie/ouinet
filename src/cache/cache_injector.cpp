#include <boost/asio/io_service.hpp>
#include <assert.h>
#include <iostream>
#include <chrono>

#include "cache_injector.h"
#include "http_desc.h"

#include <asio_ipfs.h>
#include "btree_db.h"
#include "bep44_db.h"
#include "descdb.h"
#include "publisher.h"
#include "../http_util.h"
#include "../bittorrent/dht.h"
#include "../util/scheduler.h"

using namespace std;
using namespace ouinet;
namespace bt = ouinet::bittorrent;

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

    string desc, cid;

    {
        auto slot = _scheduler->wait_for_slot(yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);

        desc = descriptor::http_create(*_ipfs_node, id, ts, rq, rs, yield[ec]);
        if (!ec && !*wd)
            cid = _ipfs_node->add(desc, yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);
    }

    auto db = get_db(db_type);
    descriptor::put_into_db( rq.target().to_string()
                           , desc, cid
                           , *db, yield[ec]);

    if (!ec && *wd) ec = asio::error::operation_aborted;

    return or_throw(yield, ec, move(desc));
}

string CacheInjector::get_descriptor( string url
                                    , DbType db_type
                                    , asio::yield_context yield)
{
    auto db = get_db(db_type);
    return descriptor::get_from_db(url, *db, *_ipfs_node, yield);
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
