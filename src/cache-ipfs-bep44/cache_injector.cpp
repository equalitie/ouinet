#include <boost/asio/io_service.hpp>
#include <assert.h>
#include <iostream>
#include <chrono>

#include "cache_injector.h"

#include <asio_ipfs.h>
#include "get_content.h"

#include "../util/bytes.h"

using namespace std;
using namespace ouinet;

namespace asio = boost::asio;
namespace sys  = boost::system;

CacheInjector::CacheInjector(asio::io_service& ios, string path_to_repo, util::Ed25519PrivateKey private_key)
    : _ipfs_node(new asio_ipfs::node(ios, path_to_repo))
    , _dht(new bittorrent::MainlineDht(ios))
    , _private_key(private_key)
    , _public_key(private_key.public_key())
    , _was_destroyed(make_shared<bool>(false))
{
    /*
     * TODO: Replace this with platform-specific dynamic interface enumeration.
     */
    asio::spawn( _ipfs_node->get_io_service(), [this] (asio::yield_context yield) {
        std::vector<asio::ip::address> addresses;
        addresses.push_back(asio::ip::address::from_string("0.0.0.0"));
        _dht->set_interfaces(addresses, yield);
    });
}

std::string CacheInjector::public_key() const
{
    return util::bytes::to_hex(_public_key.serialize());
}

void CacheInjector::insert_content_from_queue()
{
    if (_insert_queue.empty()) return;

    ++_job_count;

    auto e = move(_insert_queue.front());
    _insert_queue.pop();

    auto wd = _was_destroyed;

    auto value = move(e.value);

    _ipfs_node->add( value
                 , [this, e = move(e), wd]
                   (sys::error_code eca, string ipfs_id) {
                        if (*wd) return;

                        --_job_count;
                        insert_content_from_queue();

                        if (eca) {
                            return e.on_insert(eca, move(ipfs_id));
                        }

                        asio::spawn( _ipfs_node->get_io_service()
                                   , [ key     = move(e.key)
                                     , ipfs_id = move(ipfs_id)
                                     , ts      = e.ts
                                     , cb      = move(e.on_insert)
                                     , wd      = move(wd)
                                     , this
                                     ]
                                     (asio::yield_context yield) {
                                         if (*wd) return;

                                         /*
                                          * Use the sha1 of the URL as salt;
                                          * Use the timestamp as a version ID.
                                          */

                                         bittorrent::BencodedValue value = ipfs_id;
                                         boost::posix_time::ptime unix_epoch(boost::gregorian::date(1970, 1, 1));
                                         int64_t timestamp_unix_ms = (ts - unix_epoch).total_milliseconds();
                                         std::string key_hash = util::bytes::to_string(util::sha1(key));

                                         bittorrent::MutableDataItem item = bittorrent::MutableDataItem::sign(
                                             value,
                                             timestamp_unix_ms,
                                             key_hash,
                                             _private_key
                                         );

                                         _dht->mutable_put_start(item, yield);

                                         cb(sys::error_code(), ipfs_id);
                                     });
                   });
}

void CacheInjector::insert_content( string key
                                  , const string& value
                                  , function<void(sys::error_code, string)> cb)
{
    _insert_queue.push(
            InsertEntry{ move(key)
                       , move(value)
                       , boost::posix_time::microsec_clock::universal_time()
                       , move(cb)});

    if (_job_count >= _concurrency) {
        return;
    }

    insert_content_from_queue();
}

string CacheInjector::insert_content( string key
                                    , const string& value
                                    , asio::yield_context yield)
{
    using handler_type = typename asio::handler_type
                           < asio::yield_context
                           , void(sys::error_code, string)>::type;

    handler_type handler(yield);
    asio::async_result<handler_type> result(handler);

    insert_content(move(key), value, [h = move(handler)] (sys::error_code ec, string v) mutable {
            h(ec, move(v));
        });

    return result.get();
}

CachedContent CacheInjector::get_content(string url, asio::yield_context yield)
{
    return ouinet::get_content(_ipfs_node.get(), _dht.get(), _public_key, url, yield);
}

CacheInjector::~CacheInjector()
{
    *_was_destroyed = true;
}
