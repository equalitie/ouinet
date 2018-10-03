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
namespace ouinet { namespace bittorrent { class MainlineDht; }}

namespace ouinet {

class InjectorDb;
class Publisher;
class Scheduler;

class CacheInjector {
public:
    using OnInsert = std::function<void(boost::system::error_code, std::string)>;
    using Request  = http::request<http::string_body>;
    using Response = http::response<http::dynamic_body>;

public:
    CacheInjector( boost::asio::io_service&
                 , const boost::optional<util::Ed25519PrivateKey>& bt_publish_key
                 , fs::path path_to_repo);

    CacheInjector(const CacheInjector&) = delete;
    CacheInjector& operator=(const CacheInjector&) = delete;

    // Returns the IPNS CID of the database.
    // The database could be then looked up by e.g. pointing your browser to:
    // "https://ipfs.io/ipns/" + ipfs.id()
    std::string id() const;

    // Insert `data` into IPFS and return the resulting IPFS ID.
    //
    // TODO: This should store into a variety of systems
    // and return a set of storage URIs to the callback.
    //
    // When testing or debugging, the content can be found here:
    // "https://ipfs.io/ipfs/" + <IPFS ID>
    std::string put_data(const std::string& data, boost::asio::yield_context);

    // Gets the data stored in IPFS under `/ipfs/<ipfs_id>`.
    //
    // TODO: This should accept a generic storage URI instead.
    std::string get_data(const std::string& ipfs_id, boost::asio::yield_context);

    // Insert `content` into IPFS and store its IPFS ID under the `url` in the
    // database. On success, the function returns the file descriptor.
    std::string insert_content(Request, Response, boost::asio::yield_context);

    // Find the content previously stored by the injector under `url`.
    // The content is returned in the parameter of the callback function.
    //
    // Basically it does this: Look into the database to find the IPFS_ID
    // correspoinding to the `url`, when found, fetch the content corresponding
    // to that IPFS_ID from IPFS.
    CacheEntry get_content(std::string url, boost::asio::yield_context);

    ~CacheInjector();

private:
    std::unique_ptr<asio_ipfs::node> _ipfs_node;
    std::unique_ptr<bittorrent::MainlineDht> _bt_dht;
    std::unique_ptr<Publisher> _publisher;
    std::unique_ptr<InjectorDb> _db;
    const unsigned int _concurrency = 8;
    std::unique_ptr<Scheduler> _scheduler;
    std::shared_ptr<bool> _was_destroyed;
};

} // namespace

