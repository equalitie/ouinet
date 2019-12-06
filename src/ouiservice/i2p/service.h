#pragma once

#include <memory>

#include <boost/asio.hpp>

#include "client.h"
#include "server.h"

#include "../../ouiservice.h"

namespace i2p { namespace client {
    class ClientDestination;
}}

namespace ouinet {
namespace ouiservice {
namespace i2poui {

class Service : public std::enable_shared_from_this<Service> {
public:
    Service(const std::string& datadir, const asio::executor&);

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    Service(Service&&);
    Service& operator=(Service&&);

    ~Service();

    uint32_t  get_i2p_tunnel_ready_timeout() { return 5*60; /* 5 minutes */ };

    asio::executor get_executor() { return _exec; };

    std::shared_ptr<i2p::client::ClientDestination> get_local_destination () const { return _local_destination; };

    std::unique_ptr<Server> build_server(const std::string& private_key_filename);
    std::unique_ptr<Client> build_client(const std::string& target_id);

protected:
    asio::executor _exec;
    std::string _data_dir;
    // all client tunnels share local destination, because destination is expensive    
    std::shared_ptr<i2p::client::ClientDestination> _local_destination;
};

} // i2poui namespace
} // ouiservice namespace
} // ouinet namespace
