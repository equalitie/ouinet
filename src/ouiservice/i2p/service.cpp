#include <map>
#include <string>
#include <algorithm>
#include "service.h"

//i2p stuff
#include <I2PTunnel.h>
#include <Identity.h>
#include <Destination.h>
#include <api.h>
#include <AddressBook.h>
#include <ClientContext.h>

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

#include "../../logger.h"

static const uint16_t i2cp_port = 7454;

namespace ouinet::ouiservice::i2poui {
    // In order to prevent double init
    // This is not a complete solution.
    // We still cannot reinit a service once all of the services were terminated
    // We just can make multiple of them
    size_t init_counter;
}

static i2p::util::Mapping make_tunnel_params(size_t hops) {
    //i2pd does not support more than 8 hops
    auto h = std::to_string(std::min(hops, (size_t)8));
    return {
        { i2p::client::I2CP_PARAM_INBOUND_TUNNEL_LENGTH, h},
        { i2p::client::I2CP_PARAM_INBOUND_TUNNELS_QUANTITY, "3"},
        { i2p::client::I2CP_PARAM_OUTBOUND_TUNNEL_LENGTH, h},
        { i2p::client::I2CP_PARAM_OUTBOUND_TUNNELS_QUANTITY, "3"},
        // we set ack delay to 20 ms, because this outnet is considered as low-latency
        { i2p::client::I2CP_PARAM_STREAMING_INITIAL_ACK_DELAY, "20"}
    };
}

Service::Service(const string& datadir, const executor_type& exec, const size_t _number_of_hops_per_tunnel)
    : _exec(exec)
    , _data_dir(datadir)
    , _tunnel_params(make_tunnel_params(_number_of_hops_per_tunnel))
{
    OUI_LOG_INFO("Starting i2p tunnels");

    string datadir_arg = "--datadir=" + datadir;

    std::vector<const char*> argv({"i2pouiservice", datadir_arg.data()});

    if (init_counter++ == 0) {
        i2p::api::InitI2P(argv.size(), (char**) argv.data(), argv[0]);
        i2p::api::StartI2P();
    }


    // create local destination shared with client tunnels
    // we might change CryptoType to ECIES or to x25519 once it's available in the network
    auto keys = i2p::data::PrivateKeys::CreateRandomKeys (i2p::data::SIGNING_KEY_TYPE_EDDSA_SHA512_ED25519);

    OUI_LOG_INFO("Number of hops in I2P inbound and outbound tunnels is set to be ",
             _number_of_hops_per_tunnel);
    _local_destination = std::make_shared<i2p::client::RunnableClientDestination>(keys, false, &_tunnel_params);
    // Start destination's thread and tunnel pool
    _local_destination->Start ();

    // Access the client address book
    _i2p_address_book = &i2p::client::context.GetAddressBook();

    //we do not need to start the address book. It is seemingly has been initiated by the client context
    load_known_hosts_to_address_book();

}

Service::~Service()
{
    if (_local_destination) {
        _local_destination->Stop();
        // Doing this explicitly here because if it's destroyed _after_
        // `StopI2P` then subsequent initialization of i2pd will misbehave (the
        // next i2p c++ test will fail).
        _local_destination.reset();
    }

    if (--init_counter == 0) {
        i2p::api::StopI2P();
    }
}

std::unique_ptr<Server> Service::build_server(const std::string& private_key_filename)
{
    return std::make_unique<Server>(shared_from_this(), _data_dir + "/" + private_key_filename, get_i2p_tunnel_ready_timeout(), _exec);
}

std::unique_ptr<Client> Service::build_client(const std::string& target_id, std::shared_ptr<i2p::client::ClientDestination> destination)
{
    return std::make_unique<Client>(shared_from_this(), target_id, get_i2p_tunnel_ready_timeout(), _exec, std::move(destination));
}

void Service::load_known_hosts_to_address_book()
{
 if (_i2p_address_book) {
    std::ifstream f(_data_dir + "/" + "hosts.txt", std::ifstream::in);
    if (f.is_open ())
      {
        _i2p_address_book->LoadHostsFromStream (f, false);
        OUI_LOG_INFO("Pre-resolved host loaded!");

      }
    else
      {
        OUI_LOG_WARN("Failed to load host resolver file!");
      }
  } else {
    //inform then panic!
    OUI_LOG_ABORT("address book has not been initiated before loading!");
  }
}

