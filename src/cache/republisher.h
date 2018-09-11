#pragma once

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/system/error_code.hpp>
#include <string>
#include <memory>
#include <list>

#include "../namespaces.h"

namespace asio_ipfs { class node; }

namespace ouinet {

/*
 * When a value is published into the network it is stored onto some nodes with
 * an expiration time. Additionaly, nodes on the network come ang go, and thus
 * the value needs to be periodically re-published.
 *
 * This class periodically republished last value used in the
 * Republisher::publish function.
 */
class Republisher {
public:
    struct Loop;

public:
    Republisher(asio_ipfs::node&);
    Republisher(const Republisher&) = delete;

    void publish(const std::string&);

    ~Republisher();

private:
    asio::io_service& _ios;
    asio_ipfs::node& _ipfs_node;

    std::shared_ptr<Loop> _ipfs_loop;
};

}
