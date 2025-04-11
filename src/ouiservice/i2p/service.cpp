#include <map>
#include <string>
#include "service.h"

//i2p stuff
#include <I2CP.h>
#include <I2PTunnel.h>
#include <Log.h>
#include <Identity.h>
#include <Destination.h>
#include <api.h>
#include <AddressBook.h>

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

static const uint16_t i2cp_port = 7454;

Service::Service(const string& datadir, const AsioExecutor& exec)
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
    // Inbound and outbound tunnel length was set before to 1 in order to reduce latency.
    // The default parameters are restored now because we want to increase the level of anonymity,
    // TODO: Check that ack delay set to 20 ms is still honoured after increasing the tunnel length
    std::map<std::string, std::string> params =
    {
        { i2p::client::I2CP_PARAM_INBOUND_TUNNEL_LENGTH, "3"},
        { i2p::client::I2CP_PARAM_INBOUND_TUNNELS_QUANTITY, "3"},
        { i2p::client::I2CP_PARAM_OUTBOUND_TUNNEL_LENGTH, "3"},
        { i2p::client::I2CP_PARAM_OUTBOUND_TUNNELS_QUANTITY, "3"},
        { i2p::client::I2CP_PARAM_STREAMING_INITIAL_ACK_DELAY, "20"}
    };
    _local_destination = std::make_shared<i2p::client::RunnableClientDestination>(keys, false, &params);
    // Start destination's thread and tunnel pool
    _local_destination->Start();

    // Start address book after starting local destination
    _i2p_address_book = std::make_unique<i2p::client::AddressBook>();
    _i2p_address_book->Start();
    // TODO: verify if it is correct place to start resolver (or after i2cp starts

    // Now we are going to load our pre-defined addresses
    load_known_hosts_to_address_book();

    _i2p_address_book->StartResolvers();
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
