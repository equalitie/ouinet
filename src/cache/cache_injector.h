#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/system/error_code.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <functional>
#include <memory>
#include <queue>

#include "../namespaces.h"
#include "../util/crypto.h"
#include "cache_entry.h"

namespace asio_ipfs { class node; }
namespace ouinet { namespace bittorrent { class MainlineDht; class MutableDataItem; }}

namespace ouinet {

class Bep44InjectorIndex;
class Publisher;
class Scheduler;
class Descriptor;

class CacheInjector {
public:
    using OnInsert = std::function<void(boost::system::error_code, std::string)>;
    using Request  = http::request<http::string_body>;
    using Response = http::response<http::dynamic_body>;

    // Assorted data resulting from an insertion.
    struct InsertionResult {
        std::string key;  // key to look up descriptor
        std::string desc_data;  // serialized descriptor
        std::string desc_link;  // descriptor storage link
        std::string index_ins_data;  // index-specific data to help reinsert
        bool index_linked_desc;  // whether the descriptor is linked to by ins data
    };

private:
    // Private: use the static async `build` function
    CacheInjector( boost::asio::io_service&
                 , util::Ed25519PrivateKey bt_privkey
                 , fs::path path_to_repo
                 , std::unique_ptr<bittorrent::MainlineDht>
                 , std::unique_ptr<Bep44InjectorIndex>);

public:
    static std::unique_ptr<CacheInjector>
    build( boost::asio::io_service&
         , util::Ed25519PrivateKey bt_privkey
         , fs::path path_to_repo
         , unsigned int bep44_index_capacity
         , Cancel&
         , boost::asio::yield_context);

    CacheInjector(const CacheInjector&) = delete;
    CacheInjector& operator=(const CacheInjector&) = delete;

    // Returns the IPNS CID of the index.
    // The index could be then looked up by e.g. pointing your browser to:
    // "https://ipfs.io/ipns/" + ipfs.id()
    std::string ipfs_id() const;

    // Insert a descriptor with the given `id` for the given request and response
    // into the index, along with data in distributed storage.
    InsertionResult insert_content( const std::string& id
                                  , const Request&
                                  , Response
                                  , bool perform_io
                                  , boost::asio::yield_context);

    // Find the content previously stored by the injector under `key`.
    // The descriptor identifier and cached content are returned.
    //
    // Basically it does this: Look into the index to find the IPFS_ID
    // correspoinding to the `key`, when found, fetch the content corresponding
    // to that IPFS_ID from IPFS.
    std::pair<std::string, CacheEntry> get_content( const std::string& key
                                                  , Cancel&
                                                  , boost::asio::yield_context);

    std::string ipfs_cat( boost::string_view cid
                        , Cancel&
                        , boost::asio::yield_context);

    Descriptor bep44m_to_descriptor( const bittorrent::MutableDataItem&
                                   , Cancel&
                                   , asio::yield_context);

    bittorrent::MutableDataItem
    get_bep44m( boost::string_view key
              , Cancel&
              , boost::asio::yield_context);

    std::string get_descriptor( const std::string& key
                              , Cancel&
                              , boost::asio::yield_context);

    ~CacheInjector();

private:
    void wait_for_ready(Cancel&, boost::asio::yield_context) const;

private:
    std::unique_ptr<asio_ipfs::node> _ipfs_node;
    std::unique_ptr<bittorrent::MainlineDht> _bt_dht;
    std::unique_ptr<Publisher> _publisher;
    std::unique_ptr<Bep44InjectorIndex> _index;
    const unsigned int _concurrency = 8;
    std::unique_ptr<Scheduler> _scheduler;
    Cancel _cancel;
};

} // namespace

