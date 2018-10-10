#pragma once

#include <boost/system/error_code.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <string>

#include "../namespaces.h"
#include "resolver.h"
#include "db.h"

namespace asio_ipfs { class node; }
namespace ouinet { namespace bittorrent { class MainlineDht; }}
namespace ouinet { namespace util { class Ed25519PublicKey; }}

namespace ouinet {

class BTree;
class Publisher;

class BTreeClientDb : public ClientDb {
public:
    BTreeClientDb( asio_ipfs::node&
                 , std::string ipns
                 , bittorrent::MainlineDht& bt_dht
                 , boost::optional<util::Ed25519PublicKey> bt_publish_pubkey
                 , fs::path path_to_repo);

    std::string find(const std::string& key, asio::yield_context) override;

    boost::asio::io_service& get_io_service();

    const std::string& ipns() const { return _ipns; }
    const std::string& ipfs() const { return _ipfs; }

    const BTree* get_btree() const;

    ~BTreeClientDb();

private:
    void on_resolve(std::string cid, asio::yield_context);

private:
    const fs::path _path_to_repo;
    std::string _ipns;
    std::string _ipfs; // Last known
    asio_ipfs::node& _ipfs_node;
    std::unique_ptr<BTree> _db_map;
    Resolver _resolver;
    std::shared_ptr<bool> _was_destroyed;
};

class BTreeInjectorDb : public InjectorDb {
public:
    BTreeInjectorDb(asio_ipfs::node&, Publisher&, fs::path path_to_repo);

    std::string find(const std::string& key, asio::yield_context) override;

    void insert(std::string key, std::string value, asio::yield_context) override;

    boost::asio::io_service& get_io_service();

    const std::string& ipns() const { return _ipns; }

    ~BTreeInjectorDb();

private:
    void publish(std::string);
    void continuously_upload_db(asio::yield_context);

private:
    const fs::path _path_to_repo;
    std::string _ipns;
    asio_ipfs::node& _ipfs_node;
    Publisher& _publisher;
    std::unique_ptr<BTree> _db_map;
    std::shared_ptr<bool> _was_destroyed;
};

} // namespace

