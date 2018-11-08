#include "btree_db.h"
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
using boost::optional;

static const unsigned int BTREE_NODE_SIZE=64;

static BTree::CatOp make_cat_operation(asio_ipfs::node& ipfs_node)
{
    return [&ipfs_node] ( const BTree::Hash& hash
                        , Cancel& cancel
                        , asio::yield_context yield) {
        sys::error_code ec;
        function<void()> cancel_fn;
        auto cancel_handle = cancel.connect([&] { if (cancel_fn) cancel_fn(); });
        auto retval = ipfs_node.cat(hash, cancel_fn, yield[ec]);
        if (!ec && cancel) ec = asio::error::operation_aborted;
        return or_throw(yield, ec, move(retval));
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

static void load_db_from_disk( BTree& db_map
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

static void save_db_to_disk( const fs::path& path_to_repo
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


BTreeClientDb::BTreeClientDb( asio_ipfs::node& ipfs_node
                            , string ipns
                            , bt::MainlineDht& bt_dht
                            , optional<util::Ed25519PublicKey> bt_publish_pubkey
                            , fs::path path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipns(move(ipns))
    , _ipfs_node(ipfs_node)
    , _db_map(make_unique<BTree>( make_cat_operation(ipfs_node)
                                , nullptr
                                , nullptr
                                , BTREE_NODE_SIZE))
    , _resolver( ipfs_node
               , _ipns
               , bt_dht
               , bt_publish_pubkey
               , [this](string cid, asio::yield_context yield)
                 { on_resolve(move(cid), yield); })
    , _was_destroyed(make_shared<bool>(false))
{
    auto d = _was_destroyed;

    asio::spawn(get_io_service(), [=](asio::yield_context yield) {
            if (*d) return;

            // Already loaded?
            if (!_db_map->root_hash().empty()) return;

            load_db_from_disk(*_db_map, _path_to_repo, _ipns, yield);
        });
}

BTreeInjectorDb::BTreeInjectorDb( asio_ipfs::node& ipfs_node
                                , Publisher& publisher
                                , fs::path path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipns(ipfs_node.id())
    , _ipfs_node(ipfs_node)
    , _publisher(publisher)
    , _db_map(make_unique<BTree>( make_cat_operation(ipfs_node)
                                , make_add_operation(ipfs_node)
                                , make_remove_operation(ipfs_node)
                                , BTREE_NODE_SIZE))
    , _was_destroyed(make_shared<bool>(false))
{
    auto d = _was_destroyed;

    asio::spawn(get_io_service(), [=](asio::yield_context yield) {
            if (*d) return;

            // Already loaded?
            if (!_db_map->root_hash().empty()) return;

            load_db_from_disk(*_db_map, _path_to_repo, _ipns, yield);

            publish(_db_map->root_hash());
        });
}

const string ipfs_uri_prefix = "ipfs:/ipfs/";

void BTreeInjectorDb::insert( string key
                            , string value
                            , asio::yield_context yield)
{
    assert(!key.empty() && !value.empty());

    auto wd = _was_destroyed;
    sys::error_code ec;

    _db_map->insert(move(key), move(value), yield[ec]);

    if (!ec && *wd) ec = asio::error::operation_aborted;
    if (ec) return or_throw(yield, ec);

    publish(_db_map->root_hash());

    if (!ec && *wd) ec = asio::error::operation_aborted;
    return or_throw(yield, ec);
}

void BTreeInjectorDb::publish(string db_ipfs_id)
{
    if (db_ipfs_id.empty()) {
        return;
    }

    save_db_to_disk(_path_to_repo, _ipns, db_ipfs_id);

    _publisher.publish(move(db_ipfs_id));
}

static string query_( const string& key
                    , BTree& db
                    , Cancel& cancel
                    , asio::yield_context yield)
{
    sys::error_code ec;

    auto val = db.find(key, cancel, yield[ec]);

    if (ec) return or_throw<string>(yield, ec);

    return val;
}

string BTreeInjectorDb::find( const string& key
                            , Cancel& cancel
                            , asio::yield_context yield)
{
    return query_(key, *_db_map, cancel, yield);
}

string BTreeClientDb::find( const string& key
                          , Cancel& cancel
                          , asio::yield_context yield)
{
    return query_(key, *_db_map, cancel, yield);
}

void BTreeClientDb::on_resolve(string ipfs_id, asio::yield_context yield)
{
    auto d = _was_destroyed;

    sys::error_code ec;

    if (_ipfs == ipfs_id) return;

    _ipfs = ipfs_id;

    _db_map->load(ipfs_id, yield[ec]);

    if (*d || ec) return;

    save_db_to_disk(_path_to_repo, _ipns, ipfs_id);
}

const BTree* BTreeClientDb::get_btree() const
{
    return _db_map.get();
}

asio::io_service& BTreeClientDb::get_io_service() {
    return _ipfs_node.get_io_service();
}

asio::io_service& BTreeInjectorDb::get_io_service() {
    return _ipfs_node.get_io_service();
}

BTreeClientDb::~BTreeClientDb() {
    *_was_destroyed = true;
}

BTreeInjectorDb::~BTreeInjectorDb() {
    *_was_destroyed = true;
}
