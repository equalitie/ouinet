#pragma once

#include <memory>

#include <boost/asio.hpp>

#include "client.h"
#include "server.h"
#include "i2cp_server.h"

#include "tunneller_service.h"

#include "../../ouiservice.h"


namespace i2p::client {
    class ClientDestination;
    class AddressBook;
    class I2CPServer;
}

namespace ouinet::ouiservice::i2poui {

class Service : public std::enable_shared_from_this<Service> {
public:
    // because by default usage of i2p service is to  prioritize anominty 
    // the default tunnel length is set to 3. To makes I2P connections faster this could be reduced to 1
    Service(const std::string& datadir, const AsioExecutor&, const size_t _number_of_hops_per_tunnel = 3);

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    Service(Service&&) = delete;
    Service& operator=(Service&&) = delete;

    ~Service();

    uint32_t  get_i2p_tunnel_ready_timeout() { return 5*60; /* 5 minutes */ };

    AsioExecutor get_executor() { return _exec; };

    std::shared_ptr<i2p::client::ClientDestination> get_local_destination () const { return _local_destination; };

    std::unique_ptr<Server> build_server(const std::string& private_key_filename);
    std::unique_ptr<Client> build_client(const std::string& target_id);

    // simply start the I2CP server on the pre-defined port
    void start_i2cp_server();

    // simply the tunneller service for testing bittorent dht over i2p
    void start_tunneller_service();
  
protected:
    void load_known_hosts_to_address_book();

    AsioExecutor _exec;
    std::string _data_dir;

    // all client tunnels share local destination, because destination is expensive
    std::shared_ptr<i2p::client::ClientDestination> _local_destination;

    // We run an address book as soon as we start the the i2pd daemon simialr to i2pd client
    i2p::client::AddressBook* _i2p_address_book = nullptr;

    std::unique_ptr<i2p::client::I2CPServer> _i2cpserver;
    std::unique_ptr<TunnellerService> _i2p_tunneller;
};

} // namespaces
