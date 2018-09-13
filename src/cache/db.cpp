#include "db.h"
#include <asio_ipfs.h>
#include "publisher.h"
#include "btree.h"
#include "../or_throw.h"

#include <boost/asio/io_service.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>

#include "../logger.h"

using namespace std;
using namespace ouinet;
namespace bt = ouinet::bittorrent;

static const unsigned int BTREE_NODE_SIZE=64;

static BTree::CatOp make_cat_operation(asio_ipfs::node& ipfs_node)
{
    return [&ipfs_node] (const BTree::Hash& hash, asio::yield_context yield) {
        return ipfs_node.cat(hash, yield);
    };
}

static BTree::AddOp make_add_operation(asio_ipfs::node& ipfs_node)
{
    return [&ipfs_node] (const BTree::Value& value, asio::yield_context yield) {
        sys::error_code ec;

        auto ret = ipfs_node.add(value, yield[ec]);
        if (ec) return or_throw(yield, ec, move(ret));

        ipfs_node.pin(ret, yield[ec]);
        if (ec) return or_throw(yield, ec, move(ret));

        return ret;
    };
}

static BTree::RemoveOp make_remove_operation(asio_ipfs::node& ipfs_node)
{
    return [&ipfs_node] (const BTree::Value& hash, asio::yield_context yield) {
        ipfs_node.unpin(hash, yield);
    };
}

static string path_to_db(const fs::path& path_to_repo, const string& ipns)
{
    return (path_to_repo / ("ipfs_cache_db." + ipns)).native();
}

static void load_db( BTree& db_map
                   , const fs::path& path_to_repo
                   , const string& ipns
                   , asio::yield_context yield)
{
    string path = path_to_db(path_to_repo, ipns);

    ifstream file(path);

    if (!file.is_open()) {
        cerr << "Warning: Couldn't open " << path
             << "; Creating a new one" << endl;
        return;
    }

    try {
        string ipfs;
        file >> ipfs;

        if (ipfs.substr(0, 2) != "Qm") {
            throw runtime_error("Content doesn't start with 'Qm'");
        }

        if (ipfs.size() != asio_ipfs::node::CID_SIZE) {
            throw runtime_error("Content doesn't appear to be a CID hash");
        }

        sys::error_code ec;
        db_map.load(ipfs, yield[ec]);
    }
    catch (const std::exception& e) {
        cerr << "ERROR: parsing " << path << ": " << e.what() << endl;
    }
}

static void save_db( const fs::path& path_to_repo
                   , const string& ipns
                   , const string& ipfs)
{
    string path = path_to_db(path_to_repo, ipns);

    ofstream file(path, std::ofstream::trunc);

    if (!file.is_open()) {
        cerr << "ERROR: Saving " << path << endl;
        return;
    }

    file << ipfs;
    file.close();
}


ClientDb::ClientDb(asio_ipfs::node& ipfs_node, fs::path path_to_repo, string ipns)
    : _path_to_repo(move(path_to_repo))
    , _ipns(move(ipns))
    , _ipfs_node(ipfs_node)
    , _was_destroyed(make_shared<bool>(false))
    , _download_timer(_ipfs_node.get_io_service())
    , _db_map(make_unique<BTree>( make_cat_operation(ipfs_node)
                                , nullptr
                                , nullptr
                                , BTREE_NODE_SIZE))
{
    auto d = _was_destroyed;
    asio::spawn(get_io_service(), [=](asio::yield_context yield) {
            if (*d) return;
            load_db(*_db_map, _path_to_repo, _ipns, yield);
            if (*d) return;
            continuously_download_db(yield);
        });
}

InjectorDb::InjectorDb( asio_ipfs::node& ipfs_node
                      , Publisher& publisher
                      , fs::path path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipns(ipfs_node.id())
    , _ipfs_node(ipfs_node)
    , _publisher(publisher)
    , _has_callbacks(_ipfs_node.get_io_service())
    , _was_destroyed(make_shared<bool>(false))
    , _db_map(make_unique<BTree>( make_cat_operation(ipfs_node)
                                , make_add_operation(ipfs_node)
                                , make_remove_operation(ipfs_node)
                                , BTREE_NODE_SIZE))
{
    auto d = _was_destroyed;

    asio::spawn(get_io_service(), [=](asio::yield_context yield) {
            if (*d) return;
            load_db(*_db_map, _path_to_repo, _ipns, yield);
        });
}

const string ipfs_uri_prefix = "ipfs:/ipfs/";

void InjectorDb::update(string key, string value, asio::yield_context yield)
{
    auto wd = _was_destroyed;
    sys::error_code ec;

    _db_map->insert(move(key), move(value), yield[ec]);

    if (!ec && *wd) ec = asio::error::operation_aborted;
    if (ec) return or_throw(yield, ec);

    publish(_db_map->root_hash());

    if (!ec && *wd) ec = asio::error::operation_aborted;
    return or_throw(yield, ec);
}

void InjectorDb::publish(string db_ipfs_id)
{
    if (db_ipfs_id.empty()) {
        return;
    }

    save_db(_path_to_repo, _ipns, db_ipfs_id);

    _publisher.publish(move(db_ipfs_id));
}

static string query_(string key, BTree& db, asio::yield_context yield)
{
    sys::error_code ec;

    auto val = db.find(key, yield[ec]);

    if (ec) return or_throw<string>(yield, ec);

    return val;
}

string InjectorDb::query(string key, asio::yield_context yield)
{
    return query_(move(key), *_db_map, yield);
}

string ClientDb::query(string key, asio::yield_context yield)
{
    return query_(move(key), *_db_map, yield);
}

void ClientDb::continuously_download_db(asio::yield_context yield)
{
    auto d = _was_destroyed;

    while(true) {
        sys::error_code ec;

        LOG_DEBUG("resolving IPNS address: " + _ipns);
        auto ipfs_id = _ipfs_node.resolve(_ipns, yield[ec]);
        if (*d) return;

        if (!ec) {
          LOG_DEBUG("IPNS ID has been resolved successfully to " + ipfs_id);
          _ipfs = ipfs_id;

          _db_map->load(ipfs_id, yield[ec]);

          if (*d) return;
        } else {
          LOG_ERROR("Error in resolving IPNS: " + ec.message());
          
        }

        save_db(_path_to_repo, _ipns, ipfs_id);

        if (ec) {
            _download_timer.expires_from_now(chrono::seconds(5));
            _download_timer.async_wait(yield[ec]);
            if (*d) return;
            continue;
        }

        flush_db_update_callbacks(sys::error_code());

        _download_timer.expires_from_now(chrono::seconds(5));
        _download_timer.async_wait(yield[ec]);
        if (*d) return;
    }
}

void ClientDb::wait_for_db_update(asio::yield_context yield)
{
    using Handler = asio::handler_type<asio::yield_context,
          void(sys::error_code)>::type;

    Handler h(yield);
    asio::async_result<Handler> result(h);
    _on_db_update_callbacks.push([ h = move(h)
                                 , w = asio::io_service::work(get_io_service())
                                 ] (auto ec) mutable { h(ec); });
    result.get();
}

void ClientDb::flush_db_update_callbacks(const sys::error_code& ec)
{
    auto& q = _on_db_update_callbacks;

    while (!q.empty()) {
        auto c = move(q.front());
        q.pop();
        get_io_service().post([c = move(c), ec] () mutable { c(ec); });
    }
}

asio::io_service& ClientDb::get_io_service() {
    return _ipfs_node.get_io_service();
}

asio::io_service& InjectorDb::get_io_service() {
    return _ipfs_node.get_io_service();
}

ClientDb::~ClientDb() {
    *_was_destroyed = true;
    flush_db_update_callbacks(asio::error::operation_aborted);
}

InjectorDb::~InjectorDb() {
    *_was_destroyed = true;

    for (auto& cb : _upload_callbacks) {
        get_io_service().post([cb = move(cb)] {
                cb(asio::error::operation_aborted);
            });
    }
}
