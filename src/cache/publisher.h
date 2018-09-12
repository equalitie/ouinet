#pragma once

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/system/error_code.hpp>
#include <string>
#include <memory>
#include <list>

#include "../namespaces.h"
#include "../util/crypto.h"

namespace asio_ipfs { class node; }
namespace ouinet { namespace bittorrent { class MainlineDht; } }

namespace ouinet {

/*
 * When a value is published into the network it is stored onto some nodes with
 * an expiration time. Additionaly, nodes on the network come ang go, and thus
 * the value needs to be periodically re-published.
 *
 * This class periodically republished last value used in the
 * Publisher::publish function.
 */
class Publisher {
public:
    struct Loop;

public:
    Publisher(asio_ipfs::node&, bittorrent::MainlineDht&);
    Publisher(const Publisher&) = delete;

    void publish(const std::string&);

    ~Publisher();

private:
    asio::io_service& _ios;
    asio_ipfs::node& _ipfs_node;
    bittorrent::MainlineDht& _bt_dht;
    util::Ed25519PrivateKey _bt_private_key;

    std::shared_ptr<Loop> _ipfs_loop;
    std::shared_ptr<Loop> _bt_loop;
};

}
