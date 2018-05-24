#include <asio_ipfs.h>
#include "cache_client.h"
#include "db.h"
#include "get_content.h"
#include "../or_throw.h"

using namespace std;
using namespace ouinet;

namespace asio = boost::asio;
namespace sys  = boost::system;

unique_ptr<CacheClient> CacheClient::build( asio::io_service& ios
                                          , string ipns
                                          , string path_to_repo
                                          , function<void()>& cancel
                                          , asio::yield_context yield)
{
    using ClientP = unique_ptr<CacheClient>;

    bool canceled = false;

    cancel = [&canceled] {
        cout << "TODO: Canceling Client::build doesn't immediately stop "
             << "IO tasks" << endl;;

        canceled = true;
    };

    sys::error_code ec;
    auto ipfs_node = asio_ipfs::node::build(ios, path_to_repo, yield[ec]);

    cancel = nullptr;

    if (canceled) {
        ec = asio::error::operation_aborted;
    }

    if (ec) return or_throw<ClientP>(yield, ec);

    return ClientP(new CacheClient(move(*ipfs_node)
                  , move(ipns)
                  , move(path_to_repo)));
}

CacheClient::CacheClient( asio_ipfs::node ipfs_node
                        , string ipns
                        , string path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipfs_node(new asio_ipfs::node(move(ipfs_node)))
    , _db(new ClientDb(*_ipfs_node, _path_to_repo, ipns))
{
}

CacheClient::CacheClient( boost::asio::io_service& ios
                        , string ipns
                        , string path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipfs_node(new asio_ipfs::node(ios, _path_to_repo))
    , _db(new ClientDb(*_ipfs_node, _path_to_repo, ipns))
{
}

string CacheClient::ipfs_add(const string& data, asio::yield_context yield)
{
    return _ipfs_node->add(data, yield);
}

CachedContent CacheClient::get_content(string url, asio::yield_context yield)
{
    return ouinet::get_content(*_db, url, yield);
}

void CacheClient::wait_for_db_update(boost::asio::yield_context yield)
{
    _db->wait_for_db_update(yield);
}

void CacheClient::set_ipns(std::string ipns)
{
    _db.reset(new ClientDb(*_ipfs_node, _path_to_repo, move(ipns)));
}

std::string CacheClient::id() const
{
    return _ipfs_node->ipns_id();
}

const string& CacheClient::ipns() const
{
    return _db->ipns();
}

const string& CacheClient::ipfs() const
{
    return _db->ipfs();
}

//const Json& Client::json_db() const
//{
//    return _db->json_db();
//}

CacheClient::CacheClient(CacheClient&& other)
    : _ipfs_node(move(other._ipfs_node))
    , _db(move(other._db))
{}

CacheClient& CacheClient::operator=(CacheClient&& other)
{
    _ipfs_node = move(other._ipfs_node);
    _db = move(other._db);
    return *this;
}

CacheClient::~CacheClient() {}
