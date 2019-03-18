#pragma once

#include <boost/system/error_code.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/filesystem.hpp>

#include "../namespaces.h"
#include "../util/crypto.h"
#include "index.h"

namespace ouinet {
    namespace bittorrent {
        class MainlineDht;
        class MutableDataItem;
    }
}
namespace ouinet { namespace util { class Ed25519PublicKey; }}

namespace ouinet {

inline
std::string bep44_salt_from_key(const std::string& key)
{
    // This ensures short, fixed-size salts to be circulated
    // (as e.g. keys containing HTTP URIs may be quite long).
    auto ret = util::sha1(key);
    return std::string(ret.begin(), ret.end());
}

class Bep44EntryUpdater;

class Bep44ClientIndex : public ClientIndex {
public:
    static
    std::unique_ptr<Bep44ClientIndex>
    build( bittorrent::MainlineDht&
         , util::Ed25519PublicKey
         , const boost::filesystem::path& storage_path
         , unsigned int capacity
         , Cancel&
         , asio::yield_context);

    std::string find( const std::string& key
                    , Cancel&
                    , asio::yield_context) override;

    std::string insert_mapping( const std::string&
                              , asio::yield_context) override;

    boost::asio::io_service& get_io_service();

    ~Bep44ClientIndex();

private:
    // Private, use the async `build` fn instead.
    Bep44ClientIndex( bittorrent::MainlineDht& bt_dht
                    , util::Ed25519PublicKey bt_pubkey
                    , std::unique_ptr<Bep44EntryUpdater>);

private:
    bittorrent::MainlineDht& _bt_dht;
    util::Ed25519PublicKey _bt_pubkey;
    std::unique_ptr<Bep44EntryUpdater> _updater;
    Cancel _cancel;
};

class Bep44InjectorIndex : public InjectorIndex {
public:
    static
    std::unique_ptr<Bep44InjectorIndex>
    build( bittorrent::MainlineDht&
         , util::Ed25519PrivateKey
         , const boost::filesystem::path& storage_path
         , unsigned int capacity
         , Cancel&
         , asio::yield_context);

    std::string find( const std::string& key
                    , Cancel&
                    , asio::yield_context) override;

    std::string insert( std::string key, std::string value
                      , asio::yield_context) override;

    std::string get_insert_message( std::string key, std::string value
                                  , sys::error_code&) override;

    boost::asio::io_service& get_io_service();

    ~Bep44InjectorIndex();

private:
    bittorrent::MutableDataItem
    get_mutable_data_item(std::string key, std::string value, sys::error_code&);

    Bep44InjectorIndex( bittorrent::MainlineDht& bt_dht
                      , util::Ed25519PrivateKey bt_privkey
                      , std::unique_ptr<Bep44EntryUpdater>);

private:
    bittorrent::MainlineDht& _bt_dht;
    util::Ed25519PrivateKey _bt_privkey;
    std::unique_ptr<Bep44EntryUpdater> _updater;
    Cancel _cancel;
};

} // namespace

