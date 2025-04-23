#include <map>
#include <string>
#include <algorithm>
#include "service.h"
#include "i2cp_server.h"

//i2p stuff
#include <I2CP.h>
#include <I2PTunnel.h>
#include <Log.h>
#include <Identity.h>
#include <Destination.h>
#include <api.h>
#include <AddressBook.h>
#include <ClientContext.h>

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

static const uint16_t i2cp_port = 7454;

Service::Service(const string& datadir, const AsioExecutor& exec, const size_t _number_of_hops_per_tunnel)
    : _exec(exec)
    , _data_dir(datadir)
{
    //here we are going to read the config file and
    //set options based on those values for now we just
    //set it up by some default values;

    i2p::log::Logger().Start();

    LogPrint(eLogInfo, "Starting i2p tunnels");

    string datadir_arg = "--datadir=" + datadir;

    std::vector<const char*> argv({"i2pouiservice", datadir_arg.data()});

    i2p::api::InitI2P(argv.size(), (char**) argv.data(), argv[0]);
    i2p::api::StartI2P();

    // create local destination shared with client tunnels
    // we might change CryptoType to ECIES or to x25519 once it's available in the network
    auto keys = i2p::data::PrivateKeys::CreateRandomKeys (i2p::data::SIGNING_KEY_TYPE_EDDSA_SHA512_ED25519);

    //i2pd does not support more than 8 hops
    auto no_of_tunnel_hops_str = std::to_string(std::min(_number_of_hops_per_tunnel, (size_t)(8)));

    LogPrint(eLogInfo, "Number of hops in I2P inbound and outbound tunnels is set to be " + no_of_tunnel_hops_str);
    // we set ack delay to 20 ms, because this outnet is considered as low-latency
    std::map<std::string, std::string> params =
    {
      { i2p::client::I2CP_PARAM_INBOUND_TUNNEL_LENGTH, no_of_tunnel_hops_str},
      { i2p::client::I2CP_PARAM_INBOUND_TUNNELS_QUANTITY, "3"},
      { i2p::client::I2CP_PARAM_OUTBOUND_TUNNEL_LENGTH, no_of_tunnel_hops_str},
      { i2p::client::I2CP_PARAM_OUTBOUND_TUNNELS_QUANTITY, "3"},
      { i2p::client::I2CP_PARAM_STREAMING_INITIAL_ACK_DELAY, "20"}
    };
    _local_destination = std::make_shared<i2p::client::RunnableClientDestination>(keys, false, &params);
    // Start destination's thread and tunnel pool
    _local_destination->Start ();

    // Start address book after starting local destination
    // _i2p_address_book = std::make_unique<i2p::client::AddressBook>();
    // _i2p_address_book = &i2p::client::context.GetAddressBook();//std::make_unique<i2p::client::AddressBook>();
    // _i2p_address_book->Start ();

    // TOOD: verify if it is correct place to start resolver (or after i2cp starts

    // Now we are going to load our pre-defined addresses
    // load_known_hosts_to_address_book();

    //_i2p_address_book->StartResolvers();

}

Service::~Service()
{
    if (_local_destination) _local_destination->Stop();
    if (_i2cpserver) _i2cpserver->Stop();
    i2p::api::StopI2P();
}

std::unique_ptr<Server> Service::build_server(const std::string& private_key_filename)
{
    return std::make_unique<Server>(shared_from_this(), _data_dir + "/" + private_key_filename, get_i2p_tunnel_ready_timeout(), _exec);
}

std::unique_ptr<Client> Service::build_client(const std::string& target_id)
{
    return std::make_unique<Client>(shared_from_this(), target_id, get_i2p_tunnel_ready_timeout(), _exec);
}

void Service::start_i2cp_server() {
  _i2cpserver = std::make_unique<i2p::client::I2CPServer>("127.0.0.1",  i2cp_port, false);
  _i2cpserver->Start();
}

void Service::start_tunneller_service() {
  _i2p_tunneller = std::make_unique<TunnellerService>(shared_from_this(), _exec);
}

void Service::load_known_hosts_to_address_book()
{
  std::ifstream f(_data_dir + "/" + "hosts.txt", std::ifstream::in);
  if (f.is_open ())
    {
      _i2p_address_book->LoadHostsFromStream (f, false);
      LogPrint(eLogInfo, "Pre-resolved host loaded!");
    }
  else
    {
      LogPrint(eLogWarning, "Failed to load host resolver!");
    }
}
