#pragma once

#include <boost/system/error_code.hpp>
#include <boost/asio/io_service.hpp>

#include "../namespaces.h"
#include "../util/crypto.h"
#include "index.h"

namespace ouinet { namespace bittorrent { class MainlineDht; }}
namespace ouinet { namespace util { class Ed25519PublicKey; }}

namespace ouinet {

inline
std::array<uint8_t, 20> bep44_salt_from_key(const std::string& key)
{
    // This ensures short, fixed-size salts to be circulated
    // (as e.g. keys containing HTTP URIs may be quite long).
    return util::sha1(key);
}

class Bep44ClientIndex : public ClientIndex {
public:
    Bep44ClientIndex( bittorrent::MainlineDht& bt_dht
                    , util::Ed25519PublicKey bt_pubkey);

    std::string find( const std::string& key
                    , Cancel&
                    , asio::yield_context) override;

    std::string insert_mapping( const std::string&
                              , asio::yield_context) override;

    boost::asio::io_service& get_io_service();

    ~Bep44ClientIndex();

private:
    bittorrent::MainlineDht& _bt_dht;
    util::Ed25519PublicKey _bt_pubkey;
    std::shared_ptr<bool> _was_destroyed;
};

class Bep44InjectorIndex : public InjectorIndex {
public:
    Bep44InjectorIndex( bittorrent::MainlineDht& bt_dht
                      , util::Ed25519PrivateKey bt_privkey);

    std::string find( const std::string& key
                    , Cancel&
                    , asio::yield_context) override;

    std::string insert( std::string key, std::string value
                      , asio::yield_context) override;

    boost::asio::io_service& get_io_service();

    ~Bep44InjectorIndex();

private:
    bittorrent::MainlineDht& _bt_dht;
    util::Ed25519PrivateKey _bt_privkey;
    std::shared_ptr<bool> _was_destroyed;
};

} // namespace

