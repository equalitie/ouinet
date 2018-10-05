#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <functional>
#include <memory>
#include <string>

#include "cache_entry.h"
#include "../namespaces.h"

namespace asio_ipfs { class node; }
namespace ouinet { namespace bittorrent { class MainlineDht; }}
namespace ouinet { namespace util { class Ed25519PublicKey; }}
namespace ouinet { class BTree; }

namespace ouinet {

class BTreeClientDb;

class CacheClient {
public:
    // Construct the CacheClient without blocking the main thread as
    // constructing asio_ipfs::node takes some time.
    static std::unique_ptr<CacheClient>
    build ( boost::asio::io_service&
          , std::string ipns
          , boost::optional<util::Ed25519PublicKey> bt_pubkey
          , fs::path path_to_repo
          , std::function<void()>& cancel
          , boost::asio::yield_context);

    CacheClient(const CacheClient&) = delete;
    CacheClient& operator=(const CacheClient&) = delete;

    CacheClient(CacheClient&&);
    CacheClient& operator=(CacheClient&&);

    std::string ipfs_add(const std::string& content, boost::asio::yield_context);

    // Find the content previously stored by the injector under `url`.
    // The content is returned in the parameter of the callback function.
    //
    // Basically it does this: Look into the database to find the IPFS_ID
    // correspoinding to the `url`, when found, fetch the content corresponding
    // to that IPFS_ID from IPFS.
    CacheEntry get_content(std::string url, boost::asio::yield_context);

    std::string get_descriptor(std::string url, asio::yield_context);

    void set_ipns(std::string ipns);

    std::string ipfs_id() const;

    std::string ipns() const;
    std::string ipfs() const;

    ~CacheClient();

    const BTree* get_btree() const;

private:
    CacheClient( asio_ipfs::node
               , std::string ipns
               , boost::optional<util::Ed25519PublicKey> bt_pubkey
               , fs::path path_to_repo);

private:
    fs::path _path_to_repo;
    std::unique_ptr<asio_ipfs::node> _ipfs_node;
    std::unique_ptr<bittorrent::MainlineDht> _bt_dht;
    std::unique_ptr<BTreeClientDb> _btree_db;
};

} // namespace

