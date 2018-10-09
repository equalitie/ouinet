#pragma once

#include <boost/system/error_code.hpp>
#include <boost/asio/io_service.hpp>

#include "../namespaces.h"
#include "../util/crypto.h"
#include "db.h"

namespace ouinet { namespace bittorrent { class MainlineDht; }}
namespace ouinet { namespace util { class Ed25519PublicKey; }}

namespace ouinet {

class Bep44ClientDb : public ClientDb {
public:
    Bep44ClientDb( bittorrent::MainlineDht& bt_dht
                 , util::Ed25519PublicKey bt_pubkey);

    std::string find(const std::string& key, asio::yield_context) override;

    boost::asio::io_service& get_io_service();

    ~Bep44ClientDb();

private:
    bittorrent::MainlineDht& _bt_dht;
    util::Ed25519PublicKey _bt_pubkey;
    std::shared_ptr<bool> _was_destroyed;
};

class Bep44InjectorDb : public InjectorDb {
public:
    Bep44InjectorDb( bittorrent::MainlineDht& bt_dht
                   , util::Ed25519PrivateKey bt_privkey);

    std::string find(const std::string& key, asio::yield_context) override;

    void insert(std::string key, std::string value, asio::yield_context) override;

    boost::asio::io_service& get_io_service();

    ~Bep44InjectorDb();

private:
    bittorrent::MainlineDht& _bt_dht;
    util::Ed25519PrivateKey _bt_privkey;
    std::shared_ptr<bool> _was_destroyed;
};

} // namespace

