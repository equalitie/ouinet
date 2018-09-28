#include <boost/asio/io_service.hpp>
#include <assert.h>
#include <iostream>
#include <chrono>

#include "cache_injector.h"

#include <asio_ipfs.h>
#include "db.h"
#include "get_content.h"
#include "publisher.h"
#include "../bittorrent/dht.h"
#include "../util/scheduler.h"

using namespace std;
using namespace ouinet;
namespace bt = ouinet::bittorrent;

CacheInjector::CacheInjector
        ( asio::io_service& ios
        , const boost::optional<util::Ed25519PrivateKey>& bt_publish_key
        , fs::path path_to_repo)
    : _ipfs_node(new asio_ipfs::node(ios, (path_to_repo/"ipfs").native()))
    , _bt_dht(new bt::MainlineDht(ios))
    , _publisher(new Publisher( *_ipfs_node
                              , *_bt_dht
                              , bt_publish_key
                              , path_to_repo/"publisher"))
    , _db(new InjectorDb(*_ipfs_node, *_publisher, path_to_repo))
    , _scheduler(new Scheduler(ios, _concurrency))
    , _was_destroyed(make_shared<bool>(false))
{
    _bt_dht->set_interfaces({asio::ip::address_v4::any()});
}

string CacheInjector::id() const
{
    return _ipfs_node->id();
}

std::string CacheInjector::put_data( const std::string& data
                                   , boost::asio::yield_context yield)
{
    return insert_content("", move(data), yield);
}

string CacheInjector::insert_content( string key
                                    , const string& value
                                    , asio::yield_context yield)
{
    auto wd = _was_destroyed;

    sys::error_code ec;
    string ipfs_id;

    auto ts = boost::posix_time::microsec_clock::universal_time();

    {
        auto slot = _scheduler->wait_for_slot(yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);

        ipfs_id = _ipfs_node->add(value, yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
        if (ec) return or_throw<string>(yield, ec);
    }

    if (!key.empty()) {  // not a raw data insertion, store in database
        Json json;

        json["value"] = ipfs_id;
        json["ts"]    = boost::posix_time::to_iso_extended_string(ts) + 'Z';

        _db->update(move(key), json.dump(), yield[ec]);

        if (!ec && *wd) ec = asio::error::operation_aborted;
    }

    return or_throw(yield, ec, move(ipfs_id));
}

string CacheInjector::get_data(const string &ipfs_id, asio::yield_context yield)
{
    return _ipfs_node->cat(ipfs_id, yield);
}

CachedContent CacheInjector::get_content(string url, asio::yield_context yield)
{
    return ouinet::get_content(*_db, url, yield);
}

CacheInjector::~CacheInjector()
{
    *_was_destroyed = true;
}
