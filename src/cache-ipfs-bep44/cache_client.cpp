#include <asio_ipfs.h>
#include "cache_client.h"
#include "get_content.h"
#include "../or_throw.h"

using namespace std;
using namespace ouinet;

namespace asio = boost::asio;
namespace sys  = boost::system;

unique_ptr<CacheClient> CacheClient::build( asio::io_service& ios
                                          , util::Ed25519PublicKey public_key
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
                  , move(public_key)
                  , move(path_to_repo)));
}

CacheClient::CacheClient( asio_ipfs::node ipfs_node
                        , util::Ed25519PublicKey public_key
                        , string path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipfs_node(new asio_ipfs::node(move(ipfs_node)))
    , _dht(new bittorrent::MainlineDht(_ipfs_node->get_io_service()))
    , _public_key(move(public_key))
{
    /*
     * TODO: Replace this with platform-specific dynamic interface enumeration.
     */
    asio::spawn( _ipfs_node->get_io_service(), [this] (asio::yield_context yield) {
        std::vector<asio::ip::address> addresses;
        addresses.push_back(asio::ip::address::from_string("0.0.0.0"));
        _dht->set_interfaces(addresses, yield);
    });
}

CacheClient::CacheClient( boost::asio::io_service& ios
                        , util::Ed25519PublicKey public_key
                        , string path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipfs_node(new asio_ipfs::node(ios, _path_to_repo))
    , _dht(new bittorrent::MainlineDht(ios))
    , _public_key(move(public_key))
{
    /*
     * TODO: Replace this with platform-specific dynamic interface enumeration.
     */
    asio::spawn( _ipfs_node->get_io_service(), [this] (asio::yield_context yield) {
        std::vector<asio::ip::address> addresses;
        addresses.push_back(asio::ip::address::from_string("0.0.0.0"));
        _dht->set_interfaces(addresses, yield);
    });
}

std::string CacheClient::public_key() const
{
    return util::bytes::to_hex(_public_key.serialize());
}

/*
 * TODO: This function should be replaced with one that doesn't just seed the
 * data chunk, but also seeds the BEP44 entry.
 */
string CacheClient::ipfs_add(const string& data, asio::yield_context yield)
{
    return _ipfs_node->add(data, yield);
}

CachedContent CacheClient::get_content(string url, asio::yield_context yield)
{
    return ouinet::get_content(_ipfs_node.get(), _dht.get(), _public_key, url, yield);
}

CacheClient::CacheClient(CacheClient&& other)
    : _ipfs_node(move(other._ipfs_node))
    , _dht(move(other._dht))
    , _public_key(move(other._public_key))
{}

CacheClient& CacheClient::operator=(CacheClient&& other)
{
    _ipfs_node = move(other._ipfs_node);
    _dht = move(other._dht);
    _public_key = move(other._public_key);
    return *this;
}

CacheClient::~CacheClient() {}
