#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <functional>
#include <memory>
#include <string>

#include "cache_entry.h"
#include "index.h"
#include "../namespaces.h"

namespace asio_ipfs { class node; }
namespace ouinet { namespace bittorrent { class MainlineDht; }}
namespace ouinet { namespace util { class Ed25519PublicKey; }}
namespace ouinet { class BTree; }

namespace ouinet {

class BTreeClientIndex;
class Bep44ClientIndex;

class CacheClient {
public:
    // Construct the CacheClient without blocking the main thread as
    // constructing asio_ipfs::node takes some time.
    static std::unique_ptr<CacheClient>
    build ( boost::asio::io_service&
          , std::string ipns
          , boost::optional<util::Ed25519PublicKey> bt_pubkey
          , fs::path path_to_repo
          , unsigned int bep44_index_capacity
          , Cancel& cancel
          , boost::asio::yield_context);

    CacheClient(const CacheClient&) = delete;
    CacheClient& operator=(const CacheClient&) = delete;

    CacheClient(CacheClient&&) = delete;
    CacheClient& operator=(CacheClient&&) = delete;

    std::string ipfs_add(const std::string& content, boost::asio::yield_context);

    // Insert a signed key->descriptor mapping
    // into the index of the given type.
    // The parsing of the given data depends on the index.
    // Return a printable representation of the key resulting from insertion.
    std::string insert_mapping( const std::string&
                              , IndexType
                              , Cancel&
                              , boost::asio::yield_context);

    // Find the content previously stored by the injector under `key`.
    // The descriptor identifier and cached content are returned.
    //
    // Basically it does this: Look into the index to find the IPFS_ID
    // correspoinding to the `key`, when found, fetch the content corresponding
    // to that IPFS_ID from IPFS.
    std::pair<std::string, CacheEntry> get_content( const std::string& key
                                                  , IndexType
                                                  , Cancel&
                                                  , boost::asio::yield_context);

    std::string get_descriptor( const std::string& key
                              , IndexType
                              , Cancel&
                              , asio::yield_context);

    std::string descriptor_from_path( const std::string& desc_path
                                    , Cancel&
                                    , asio::yield_context);

    void set_ipns(std::string ipns);

    std::string ipfs_id() const;

    std::string ipns() const;
    std::string ipfs() const;

    void wait_for_ready(Cancel&, boost::asio::yield_context) const;

    ~CacheClient();

    const BTree* get_btree() const;

private:
    // Private, use the static `build` function instead
    CacheClient( asio_ipfs::node
               , std::string ipns
               , boost::optional<util::Ed25519PublicKey> bt_pubkey
               , std::unique_ptr<bittorrent::MainlineDht>
               , std::unique_ptr<Bep44ClientIndex>
               , fs::path path_to_repo);

    ClientIndex* get_index(IndexType);

private:
    fs::path _path_to_repo;
    std::unique_ptr<asio_ipfs::node> _ipfs_node;
    std::unique_ptr<bittorrent::MainlineDht> _bt_dht;
    std::unique_ptr<BTreeClientIndex> _btree_index;
    std::unique_ptr<Bep44ClientIndex> _bep44_index;
};

} // namespace

