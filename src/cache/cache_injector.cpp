#include <boost/asio/io_service.hpp>
#include <assert.h>
#include <iostream>
#include <chrono>

#include "cache_injector.h"
#include "http_desc.h"

#include <asio_ipfs.h>
#include "btree_index.h"
#include "bep44_index.h"
#include "descidx.h"
#include "publisher.h"
#include "ipfs_util.h"
#include "../async_sleep.h"
#include "../bittorrent/dht.h"
#include "../logger.h"
#include "../util/scheduler.h"

using namespace std;
using namespace ouinet;
namespace bt = ouinet::bittorrent;

CacheInjector::CacheInjector
        ( asio::io_service& ios
        , util::Ed25519PrivateKey bt_privkey
        , fs::path path_to_repo
        , bool enable_btree
        , bool enable_bep44)
    : _ipfs_node(new asio_ipfs::node(ios, (path_to_repo/"ipfs").native()))
    , _bt_dht(new bt::MainlineDht(ios))  // used by either B-tree over BEP44, or BEP44
    , _scheduler(new Scheduler(ios, _concurrency))
    , _was_destroyed(make_shared<bool>(false))
{
    // TODO: Check that at least one index is enabled.
    if (enable_btree) {
        _publisher.reset(new Publisher(*_ipfs_node, *_bt_dht, bt_privkey));
        _btree_index.reset(new BTreeInjectorIndex(*_ipfs_node, *_publisher, path_to_repo));
    }
    _bt_dht->set_interfaces({asio::ip::address_v4::any()});
    if (enable_bep44) {
        _bep44_index.reset(new Bep44InjectorIndex(*_bt_dht, bt_privkey));
    }
}

string CacheInjector::ipfs_id() const
{
    return _ipfs_node->id();
}

InjectorIndex* CacheInjector::get_index(IndexType index_type) const
{
    switch (index_type) {
        case IndexType::btree: return _btree_index.get();
        case IndexType::bep44: return _bep44_index.get();
    }

    return nullptr;
}

CacheInjector::InsertionResult
CacheInjector::insert_content( const string& id
                             , const Request& rq
                             , const Response& rs
                             , IndexType index_type
                             , asio::yield_context yield)
{
    auto index = get_index(index_type);
    if (!index)
        return or_throw<CacheInjector::InsertionResult>
            (yield, asio::error::operation_not_supported);

    auto wd = _was_destroyed;

    // Wraps IPFS add operation to wait for a slot first
    auto ipfs_add = [&](auto data, auto yield) {
        sys::error_code ec;
        auto slot = _scheduler->wait_for_slot(yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);

        auto cid = _ipfs_node->add(data, yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        return or_throw(yield, ec, move(cid));
    };

    sys::error_code ec;

    // Prepare and create descriptor
    auto ts = boost::posix_time::microsec_clock::universal_time();
    auto desc = descriptor::http_create( id, ts
                                       , rq, rs
                                       , ipfs_add
                                       , yield[ec]);
    if (!ec && *wd) ec = asio::error::operation_aborted;
    if (ec) return or_throw<CacheInjector::InsertionResult>(yield, ec);

    // Store descriptor
    auto key = key_from_http_req(rq);
    auto cid_insdata = descriptor::put_into_index
        (key, desc, *index, ipfs_add, yield[ec]);
    if (!ec && *wd) ec = asio::error::operation_aborted;

    CacheInjector::InsertionResult ret
        { move(key), move(desc)
        , "/ipfs/" + cid_insdata.first, move(cid_insdata.second)};
    return or_throw(yield, ec, move(ret));
}

string CacheInjector::get_descriptor( const string& key
                                    , IndexType index_type
                                    , Cancel& cancel
                                    , asio::yield_context yield)
{
    auto index = get_index(index_type);
    if (!index)
        return or_throw<string>(yield, asio::error::operation_not_supported);

    return descriptor::get_from_index
        ( key, *index, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield);
}

pair<string, CacheEntry>
CacheInjector::get_content( const string& key
                          , IndexType index_type
                          , Cancel& cancel
                          , asio::yield_context yield)
{
    sys::error_code ec;

    string desc_data = get_descriptor(key, index_type, cancel, yield[ec]);

    if (ec) return or_throw<pair<string, CacheEntry>>(yield, ec);

    return descriptor::http_parse
        ( desc_data, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield);
}

void
CacheInjector::wait_for_ready(Cancel& cancel, asio::yield_context yield) const
{
    // TODO: Wait for IPFS cache to be ready, if needed.
    if (_bep44_index) {
        LOG_DEBUG("BEP44 index: waiting for BitTorrent DHT bootstrap...");
        _bt_dht->wait_all_ready(yield, cancel);
        LOG_DEBUG("BEP44 index: bootstrapped BitTorrent DHT");  // used by integration tests
    }
}

CacheInjector::~CacheInjector()
{
    *_was_destroyed = true;
}
