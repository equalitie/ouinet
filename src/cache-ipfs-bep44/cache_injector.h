#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/system/error_code.hpp>
#include <functional>
#include <memory>
#include <queue>

#include "cached_content.h"

#include "../bittorrent/dht.h"
#include "../util/crypto.h"

namespace asio_ipfs { class node; }

namespace ouinet {

class CacheInjector {
public:
    using OnInsert = std::function<void(boost::system::error_code, std::string)>;

private:
    struct InsertEntry {
        std::string key;
        std::string value;
        boost::posix_time::ptime ts;
        OnInsert on_insert;
    };

public:
    CacheInjector(boost::asio::io_service&, std::string path_to_repo, util::Ed25519PrivateKey private_key);

    CacheInjector(const CacheInjector&) = delete;
    CacheInjector& operator=(const CacheInjector&) = delete;

    // Returns a hex representation of the public key of the cache.
    std::string public_key() const;

    // Insert `content` into IPFS and store its IPFS ID under the `url` in the
    // database. The IPFS ID is also returned as a parameter to the callback
    // function.
    //
    // When testing or debugging, the content can be found here:
    // "https://ipfs.io/ipfs/" + <IPFS ID>
    void insert_content( std::string url
                       , const std::string& content
                       , OnInsert);

    std::string insert_content( std::string url
                              , const std::string& content
                              , boost::asio::yield_context);

    // Find the content previously stored by the injector under `url`.
    // The content is returned in the parameter of the callback function.
    //
    // Basically it does this: Look into the database to find the IPFS_ID
    // correspoinding to the `url`, when found, fetch the content corresponding
    // to that IPFS_ID from IPFS.
    CachedContent get_content(std::string url, boost::asio::yield_context);

    ~CacheInjector();

private:
    void insert_content_from_queue();

private:
    std::unique_ptr<asio_ipfs::node> _ipfs_node;
    std::unique_ptr<bittorrent::MainlineDht> _dht;
    util::Ed25519PrivateKey _private_key;
    util::Ed25519PublicKey _public_key;
    std::queue<InsertEntry> _insert_queue;
    const unsigned int _concurrency = 8;
    unsigned int _job_count = 0;
    std::shared_ptr<bool> _was_destroyed;
};

} // namespace

