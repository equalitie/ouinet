#include "btree_index.h"
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
// This should be enough to insert small values
// and IPFS links to bigger values.
static const unsigned int BTREE_DATA_MAX_SIZE=128;

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

static string path_to_index(const fs::path& path_to_repo, const string& ipns)
{
    return (path_to_repo / ("ipfs_cache_index." + ipns)).native();
}

static void load_index_from_disk( BTree& index_map
                                , const fs::path& path_to_repo
                                , const string& ipns
                                , asio::yield_context yield)
{
    string path = path_to_index(path_to_repo, ipns);

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
        index_map.load(ipfs, yield[ec]);
    }
    catch (const std::exception& e) {
        cerr << "ERROR: parsing " << path << ": " << e.what() << endl;
    }
}

static void save_index_to_disk( const fs::path& path_to_repo
                              , const string& ipns
                              , const string& ipfs)
{
    string path = path_to_index(path_to_repo, ipns);

    ofstream file(path, std::ofstream::trunc);

    if (!file.is_open()) {
        cerr << "ERROR: Saving " << path << endl;
        return;
    }

    file << ipfs;
    file.close();
}


BTreeClientIndex::BTreeClientIndex( asio_ipfs::node& ipfs_node
                                  , string ipns
                                  , bt::MainlineDht& bt_dht
                                  , optional<util::Ed25519PublicKey> bt_publish_pubkey
                                  , fs::path path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipns(move(ipns))
    , _ipfs_node(ipfs_node)
    , _index_map(make_unique<BTree>( make_cat_operation(ipfs_node)
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
            if (!_index_map->root_hash().empty()) return;

            load_index_from_disk(*_index_map, _path_to_repo, _ipns, yield);
        });
}

BTreeInjectorIndex::BTreeInjectorIndex( asio_ipfs::node& ipfs_node
                                      , Publisher& publisher
                                      , fs::path path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipns(ipfs_node.id())
    , _ipfs_node(ipfs_node)
    , _publisher(publisher)
    , _index_map(make_unique<BTree>( make_cat_operation(ipfs_node)
                                   , make_add_operation(ipfs_node)
                                   , make_remove_operation(ipfs_node)
                                   , BTREE_NODE_SIZE))
    , _was_destroyed(make_shared<bool>(false))
{
    auto d = _was_destroyed;

    asio::spawn(get_io_service(), [=](asio::yield_context yield) {
            if (*d) return;

            // Already loaded?
            if (!_index_map->root_hash().empty()) return;

            load_index_from_disk(*_index_map, _path_to_repo, _ipns, yield);

            publish(_index_map->root_hash());
        });
}

string BTreeInjectorIndex::insert( string key
                                 , string value
                                 , asio::yield_context yield)
{
    assert(!key.empty() && !value.empty());
    if (value.size() > BTREE_DATA_MAX_SIZE)
        return or_throw<string>(yield, asio::error::message_size);

    auto wd = _was_destroyed;
    sys::error_code ec;

    _index_map->insert(move(key), move(value), yield[ec]);

    if (!ec && *wd) ec = asio::error::operation_aborted;
    if (ec) return or_throw<string>(yield, ec);

    publish(_index_map->root_hash());

    if (!ec && *wd) ec = asio::error::operation_aborted;
    // No data is returned to help with reinsertion.
    return or_throw(yield, ec, "");
}

void BTreeInjectorIndex::publish(string index_ipfs_id)
{
    if (index_ipfs_id.empty()) {
        return;
    }

    save_index_to_disk(_path_to_repo, _ipns, index_ipfs_id);

    _publisher.publish(move(index_ipfs_id));
}

static string query_( const string& key
                    , BTree& index
                    , Cancel& cancel
                    , asio::yield_context yield)
{
    sys::error_code ec;

    auto val = index.find(key, cancel, yield[ec]);

    if (ec) return or_throw<string>(yield, ec);

    return val;
}

string BTreeInjectorIndex::find( const string& key
                            , Cancel& cancel
                            , asio::yield_context yield)
{
    return query_(key, *_index_map, cancel, yield);
}

string BTreeClientIndex::find( const string& key
                             , Cancel& cancel
                             , asio::yield_context yield)
{
    return query_(key, *_index_map, cancel, yield);
}

void BTreeClientIndex::on_resolve(string ipfs_id, asio::yield_context yield)
{
    auto d = _was_destroyed;

    sys::error_code ec;

    if (_ipfs == ipfs_id) return;

    _ipfs = ipfs_id;

    _index_map->load(ipfs_id, yield[ec]);

    if (*d || ec) return;

    save_index_to_disk(_path_to_repo, _ipns, ipfs_id);
}

const BTree* BTreeClientIndex::get_btree() const
{
    return _index_map.get();
}

asio::io_service& BTreeClientIndex::get_io_service() {
    return _ipfs_node.get_io_service();
}

asio::io_service& BTreeInjectorIndex::get_io_service() {
    return _ipfs_node.get_io_service();
}

BTreeClientIndex::~BTreeClientIndex() {
    *_was_destroyed = true;
}

BTreeInjectorIndex::~BTreeInjectorIndex() {
    *_was_destroyed = true;
}
