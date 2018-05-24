#pragma once

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/spawn.hpp>
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
    Republisher(asio_ipfs::node&);

    void publish( const std::string&
                , std::function<void(boost::system::error_code)>);

    void publish(const std::string&, asio::yield_context);

    ~Republisher();

private:
    void start_publishing();

private:
    std::shared_ptr<bool> _was_destroyed;
    asio_ipfs::node& _ipfs_node;
    boost::asio::steady_timer _timer;
    bool _is_publishing = false;
    std::string _to_publish;
    std::list<std::function<void(boost::system::error_code)>> _callbacks;
};

}
