#pragma once

#include <boost/system/error_code.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <string>

#include "../namespaces.h"
#include "../util/crypto.h"
#include "resolver.h"

namespace ouinet { namespace bittorrent { class MainlineDht; }}
namespace ouinet { namespace util { class Ed25519PublicKey; }}

namespace ouinet {

class Bep44ClientDb {
public:
    Bep44ClientDb( bittorrent::MainlineDht& bt_dht
                 , util::Ed25519PublicKey bt_pubkey);

    std::string find(const std::string& key, asio::yield_context);

    boost::asio::io_service& get_io_service();

    ~Bep44ClientDb();

private:
    bittorrent::MainlineDht& _bt_dht;
    util::Ed25519PublicKey _bt_pubkey;
    std::shared_ptr<bool> _was_destroyed;
};

class Bep44InjectorDb {
public:
    Bep44InjectorDb( bittorrent::MainlineDht& bt_dht
                   , util::Ed25519PrivateKey bt_privkey);

    std::string find(const std::string& key, asio::yield_context);

    void insert(std::string key, std::string value, asio::yield_context);

    boost::asio::io_service& get_io_service();

    ~Bep44InjectorDb();

private:
    bittorrent::MainlineDht& _bt_dht;
    util::Ed25519PrivateKey _bt_privkey;
    std::shared_ptr<bool> _was_destroyed;
};

} // namespace

