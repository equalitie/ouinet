#include <asio_ipfs.h>
#include "cache_client.h"
#include "btree_index.h"
#include "bep44_index.h"
#include "cache_entry.h"
#include "descidx.h"
#include "http_desc.h"
#include "ipfs_util.h"
#include "../or_throw.h"
#include "../async_sleep.h"
#include "../bittorrent/dht.h"
#include "../logger.h"
#include "../util/crypto.h"

using namespace std;
using namespace ouinet;

namespace asio = boost::asio;
namespace sys  = boost::system;
namespace bt   = ouinet::bittorrent;

using boost::optional;

unique_ptr<CacheClient>
CacheClient::build( asio::io_service& ios
                  , string ipns
                  , optional<util::Ed25519PublicKey> bt_pubkey
                  , fs::path path_to_repo
                  , bool autoseed_updated
                  , unsigned int bep44_index_capacity
                  , Cancel& cancel
                  , asio::yield_context yield)
{
    using ClientP = unique_ptr<CacheClient>;

    sys::error_code ec;

    unique_ptr<asio_ipfs::node> ipfs_node;

    {
        auto slot = cancel.connect([&] {
            cerr << "TODO: Canceling CacheClient::build doesn't immediately stop "
                 << "IO tasks\n";
        });

        ipfs_node = asio_ipfs::node::build( ios
                                          , true
                                          , (path_to_repo/"ipfs").native()
                                          , yield[ec]);
    }

    if (cancel) ec = asio::error::operation_aborted;
    if (ec) return or_throw<ClientP>(yield, ec);

    ClientIndex::UpdatedHook updated_hook = [](auto o, auto n, auto& c, auto y){};
    if (autoseed_updated) {
        updated_hook = [&] (auto o, auto n, auto& c, auto y) {
            auto ipfs_load = IPFS_LOAD_FUNC(*ipfs_node);
            sys::error_code ec;
            Cancel cancel(c);

            // Fetch and decode new descriptor.
            auto desc_data = descriptor::from_path(n, ipfs_load, cancel, y[ec]);
            if (ec || cancel) return;
            auto desc = Descriptor::deserialize(desc_data);
            if (!desc) return;

            // Fetch data pointed by new descriptor.
            // TODO: check if it matches that of old descriptor
            ec = sys::error_code();
            auto data = ipfs_load(desc->body_link, cancel, y[ec]);
            if (ec || cancel) return;

            LOG_DEBUG("Fetched data from updated index entry: url=", desc->url)
        };
    }

    auto bt_dht = make_unique<bt::MainlineDht>(ios);

    bt_dht->set_interfaces({asio::ip::address_v4::any()});

    unique_ptr<Bep44ClientIndex> bep44_index;

    if (bt_pubkey) {
        bep44_index = Bep44ClientIndex::build(*bt_dht, *bt_pubkey
                                             , path_to_repo / "bep44-index"
                                             , bep44_index_capacity
                                             , move(updated_hook)
                                             , cancel
                                             , yield[ec]);

        assert(!cancel || ec == asio::error::operation_aborted);
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<ClientP>(yield, ec);
    }

    return ClientP(new CacheClient( move(*ipfs_node)
                                  , move(ipns)
                                  , std::move(bt_pubkey)
                                  , std::move(bt_dht)
                                  , std::move(bep44_index)
                                  , move(path_to_repo)));
}

// private
CacheClient::CacheClient( asio_ipfs::node ipfs_node
                        , string ipns
                        , optional<util::Ed25519PublicKey> bt_pubkey
                        , unique_ptr<bittorrent::MainlineDht> bt_dht
                        , unique_ptr<Bep44ClientIndex> bep44_index
                        , fs::path path_to_repo)
    : _path_to_repo(move(path_to_repo))
    , _ipfs_node(new asio_ipfs::node(move(ipfs_node)))
    , _bt_dht(move(bt_dht))
    , _bep44_index(move(bep44_index))
{
    if (!ipns.empty()) {
        _btree_index.reset(new BTreeClientIndex( *_ipfs_node
                                               , ipns
                                               , *_bt_dht
                                               , bt_pubkey
                                               , _path_to_repo));
    }
}

const BTree* CacheClient::get_btree() const
{
    if (!_ipfs_node) return nullptr;
    if (!_btree_index) return nullptr;
    return _btree_index->get_btree();
}

string CacheClient::ipfs_add(const string& data, asio::yield_context yield)
{
    return _ipfs_node->add(data, yield);
}

string CacheClient::insert_mapping( const boost::string_view target
                                  , const std::string& ins_data
                                  , IndexType index_type
                                  , Cancel& cancel
                                  , boost::asio::yield_context yield)
{
    auto index = get_index(index_type);

    if (!index) return or_throw<string>( yield
                                       , asio::error::operation_not_supported);

    return index->insert_mapping(target, ins_data, cancel, yield);
}

ClientIndex* CacheClient::get_index(IndexType index_type)
{
    switch (index_type) {
        case IndexType::btree: return _btree_index.get();
        case IndexType::bep44: return _bep44_index.get();
    }

    assert(0);
    return nullptr;
}

string CacheClient::get_descriptor( const string& key
                                  , IndexType index_type
                                  , Cancel& cancel
                                  , asio::yield_context yield)
{
    auto index = get_index(index_type);

    if (!index) return or_throw<string>(yield, asio::error::not_found);

    sys::error_code ec;

    string desc_path = index->find(key, cancel, yield[ec]);

    return_or_throw_on_error(yield, cancel, ec, string());

    return descriptor_from_path(desc_path, cancel, yield);
}

string CacheClient::descriptor_from_path( const string& desc_path
                                        , Cancel& cancel
                                        , asio::yield_context yield)
{
    return descriptor::from_path
        ( desc_path, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield);
}

pair<string, CacheEntry>
CacheClient::get_content( const string& key
                        , IndexType index_type
                        , Cancel& cancel
                        , asio::yield_context yield)
{
    sys::error_code ec;

    string desc_data = get_descriptor(key, index_type, cancel, yield[ec]);

    if (ec) return or_throw<pair<string, CacheEntry>>(yield, ec);

    return descriptor::http_parse
        ( desc_data, IPFS_LOAD_FUNC(*_ipfs_node), cancel, yield);
}

void CacheClient::set_ipns(std::string ipns)
{
    assert(0 && "TODO");
    //_btree_index.reset(new ClientIndex(*_ipfs_node, _path_to_repo, move(ipns)));
}

std::string CacheClient::ipfs_id() const
{
    return _ipfs_node->id();
}

string CacheClient::ipns() const
{
    if (!_btree_index) return {};
    return _btree_index->ipns();
}

string CacheClient::ipfs() const
{
    if (!_btree_index) return {};
    return _btree_index->ipfs();
}

void
CacheClient::wait_for_ready(Cancel& cancel, asio::yield_context yield) const
{
    // TODO: Wait for IPFS cache to be ready, if needed.
    LOG_DEBUG("BEP44 index: waiting for BitTorrent DHT bootstrap...");
    _bt_dht->wait_all_ready(yield, cancel);
    LOG_DEBUG("BEP44 index: bootstrapped BitTorrent DHT");  // used by integration tests
}

CacheClient::~CacheClient() {}
