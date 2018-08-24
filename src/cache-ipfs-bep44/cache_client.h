#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/system/error_code.hpp>
#include <functional>
#include <memory>
#include <string>

#include "cached_content.h"

#include "../bittorrent/dht.h"
#include "../util/crypto.h"

namespace asio_ipfs {
    class node;
}

namespace ouinet {

class CacheClient {
public:
    static std::unique_ptr<CacheClient> build( boost::asio::io_service&
                                             , util::Ed25519PublicKey public_key
                                             , std::string path_to_repo
                                             , std::function<void()>& cancel
                                             , boost::asio::yield_context);

    // This constructor may do repository initialization disk IO and as such
    // may block for a second or more. If that is undesirable, use the above
    // static async `build` function instead.
    CacheClient( boost::asio::io_service&
               , util::Ed25519PublicKey public_key
               , std::string path_to_repo);

    CacheClient(const CacheClient&) = delete;
    CacheClient& operator=(const CacheClient&) = delete;

    CacheClient(CacheClient&&);
    CacheClient& operator=(CacheClient&&);

    // Returns a hex representation of the public key of the cache.
    std::string public_key() const;

    std::string ipfs_add(const std::string& content, boost::asio::yield_context);

    // Find the content previously stored by the injector under `url`.
    // The content is returned in the parameter of the callback function.
    CachedContent get_content(std::string url, boost::asio::yield_context);

    ~CacheClient();

private:
    CacheClient(asio_ipfs::node, util::Ed25519PublicKey public_key, std::string path_to_repo);

private:
    std::string _path_to_repo;
    std::unique_ptr<asio_ipfs::node> _ipfs_node;
    std::unique_ptr<bittorrent::MainlineDht> _dht;
    util::Ed25519PublicKey _public_key;
};

} // namespace

