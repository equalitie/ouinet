#pragma once

#include <boost/system/error_code.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem.hpp>

#include "../namespaces.h"
#include "../util/crypto.h"
#include "../util/signal.h"

namespace ouinet {
    namespace bittorrent {
        class MainlineDht;
        class MutableDataItem;
    }
}
namespace ouinet { namespace util { class Ed25519PublicKey; }}

namespace ouinet {

inline
std::string bep44_salt_from_key(boost::string_view key)
{
    // This ensures short, fixed-size salts to be circulated
    // (as e.g. keys containing HTTP URIs may be quite long).
    auto ret = util::sha1(key);
    return std::string(ret.begin(), ret.end());
}

class Bep44EntryUpdater;

class Bep44ClientIndex {
public:
    using UpdatedHook = std::function<bool( std::string old, std::string new_
                                          , Cancel&, asio::yield_context)>;

    static
    std::unique_ptr<Bep44ClientIndex>
    build( bittorrent::MainlineDht&
         , util::Ed25519PublicKey
         , const boost::filesystem::path& storage_path
         , unsigned int capacity
         , Cancel&
         , asio::yield_context);

    // If set, when the index detects a change in an entry that this client is
    // publishing, this function is called with the old and new values in the
    // index, and it returns whether it considers the new value usable for
    // further processing (e.g. storage or publishing).  It should *not*
    // propagate an error code.
    void updated_hook(UpdatedHook);

    std::string find( const std::string& key
                    , Cancel&
                    , asio::yield_context);

    std::string insert_mapping( const boost::string_view target
                              , const std::string&
                              , Cancel&
                              , asio::yield_context);

    std::string insert_mapping( const boost::string_view target
                              , bittorrent::MutableDataItem
                              , Cancel&
                              , asio::yield_context);

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

class Bep44InjectorIndex {
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
                    , asio::yield_context);

    bittorrent::MutableDataItem
    find_bep44m( boost::string_view key
               , Cancel& cancel_
               , asio::yield_context yield);

    std::string insert( std::string key, std::string value
                      , asio::yield_context);

    std::string get_insert_message( std::string key, std::string value
                                  , sys::error_code&);

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

