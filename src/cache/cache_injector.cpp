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

    switch (db_type) {
        case DbType::btree:
            _btree_db->insert(move(key), desc.first, yield[ec]);
            break;
        case DbType::bep44:
            _bep44_db->insert(move(key), desc.first, yield[ec]);
            break;
    }

    if (!ec && *wd) ec = asio::error::operation_aborted;

    return or_throw(yield, ec, desc.second);
}

CacheEntry CacheInjector::get_content( string url
                                     , DbType db_type
                                     , asio::yield_context yield)
{
    sys::error_code ec;
    string desc_data;

    switch (db_type) {
        case DbType::btree:
            desc_data = _btree_db->find(url, yield[ec]);
            break;
        case DbType::bep44:
            desc_data = _bep44_db->find(url, yield[ec]);
            break;
    }

    if (ec) return or_throw<CacheEntry>(yield, ec);

    return descriptor::http_parse(*_ipfs_node, desc_data, yield);
}

CacheInjector::~CacheInjector()
{
    *_was_destroyed = true;
}
