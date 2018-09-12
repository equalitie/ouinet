#pragma once

#include <boost/system/error_code.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <string>
#include <queue>
#include <list>
#include <json.hpp>

#include "../namespaces.h"
#include "../util/condition_variable.h"

namespace asio_ipfs { class node; }
namespace ouinet { namespace bittorrent { class MainlineDht; }}

namespace ouinet {

class BTree;
class Republisher;
using Json = nlohmann::json;

class ClientDb {
    using OnDbUpdate = std::function<void(const sys::error_code&)>;

public:
    ClientDb(asio_ipfs::node&, fs::path path_to_repo, std::string ipns);

    std::string query(std::string key, asio::yield_context);

    boost::asio::io_service& get_io_service();

    const std::string& ipns() const { return _ipns; }
    const std::string& ipfs() const { return _ipfs; }

    void wait_for_db_update(boost::asio::yield_context);

    asio_ipfs::node& ipfs_node() { return _ipfs_node; }

    ~ClientDb();

private:
    void merge(const Json&);

    Json download_database(const std::string& ipns, sys::error_code&, asio::yield_context);
    void continuously_download_db(asio::yield_context);

    void flush_db_update_callbacks(const sys::error_code&);

private:
    const fs::path _path_to_repo;
    std::string _ipns;
    std::string _ipfs; // Last known
    asio_ipfs::node& _ipfs_node;
    std::shared_ptr<bool> _was_destroyed;
    asio::steady_timer _download_timer;
    std::queue<OnDbUpdate> _on_db_update_callbacks;
    std::unique_ptr<BTree> _db_map;
};

class InjectorDb {
public:
    InjectorDb(asio_ipfs::node&, Republisher&, fs::path path_to_repo);

    void update(std::string key, std::string content_hash, asio::yield_context);

    std::string query(std::string key, asio::yield_context);

    boost::asio::io_service& get_io_service();

    const std::string& ipns() const { return _ipns; }

    asio_ipfs::node& ipfs_node() { return _ipfs_node; }

    ~InjectorDb();

private:
    void upload_database(asio::yield_context);
    void continuously_upload_db(asio::yield_context);

private:
    const fs::path _path_to_repo;
    std::string _ipns;
    asio_ipfs::node& _ipfs_node;
    Republisher& _republisher;
    ConditionVariable _has_callbacks;
    std::list<std::function<void(sys::error_code)>> _upload_callbacks;
    std::shared_ptr<bool> _was_destroyed;
    std::unique_ptr<BTree> _db_map;
};

} // namespace

