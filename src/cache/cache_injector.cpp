#include <boost/asio/io_service.hpp>
#include <assert.h>
#include <iostream>
#include <chrono>

#include "cache_injector.h"

#include <asio_ipfs.h>
#include "db.h"
#include "get_content.h"

using namespace std;
using namespace ouinet;

namespace asio = boost::asio;
namespace sys  = boost::system;

CacheInjector::CacheInjector(asio::io_service& ios, string path_to_repo)
    : _ipfs_node(new asio_ipfs::node(ios, path_to_repo))
    , _db(new InjectorDb(*_ipfs_node, path_to_repo))
    , _was_destroyed(make_shared<bool>(false))
{
}

string CacheInjector::id() const
{
    return _ipfs_node->id();
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

                                         sys::error_code ec;

                                         if (!key.empty()) {  // not a raw data insertion, store in database
                                             Json json;

                                             json["value"] = ipfs_id;
                                             json["ts"]    = boost::posix_time::to_iso_extended_string(ts) + 'Z';

                                             _db->update(move(key), json.dump(), yield[ec]);
                                         }
                                         cb(ec, ipfs_id);
                                     });
                   });
}

void CacheInjector::put_data( const string& data
                            , function<void(sys::error_code, string)> cb)
{
    insert_content("", move(data), move(cb));
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
