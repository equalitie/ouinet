#include <map>
#include <string>
#include "service.h"

//i2p stuff
#include <I2PTunnel.h>
#include <Log.h>
#include <Identity.h>
#include <Destination.h>
#include <api.h>

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

Service::Service(const string& datadir, const boost::asio::executor& exec)
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
    // here we override default parameter, because we need to bypass censorship, rather then provide high-level of anominty
    // hence we can set tunnel length to 1 (rather than 3 by default), that makes I2P connections faster
    // we set ack delay to 20 ms, because this outnet is considered as low-latency
    std::map<std::string, std::string> params =
    {
        { i2p::client::I2CP_PARAM_INBOUND_TUNNEL_LENGTH, "1"},
        { i2p::client::I2CP_PARAM_INBOUND_TUNNELS_QUANTITY, "3"},
        { i2p::client::I2CP_PARAM_OUTBOUND_TUNNEL_LENGTH, "1"},
        { i2p::client::I2CP_PARAM_OUTBOUND_TUNNELS_QUANTITY, "3"},
        { i2p::client::I2CP_PARAM_STREAMING_INITIAL_ACK_DELAY, "20"}
    };
    _local_destination = std::make_shared<i2p::client::ClientDestination>(keys, false, &params);
    // start destination's thread and tunnel pool
    _local_destination->Start ();
}

Service::Service(Service&& other)
    : _exec(std::move(other._exec))
    , _data_dir(std::move(other._data_dir))
{}

Service& Service::operator=(Service&& other)
{
    _data_dir = std::move(other._data_dir);
    return *this;
}

Service::~Service()
{
    if (_local_destination) _local_destination->Stop ();    
    i2p::api::StopI2P();
}

std::unique_ptr<Server> Service::build_server(const std::string& private_key_filename)
{
    return std::unique_ptr<Server>(new Server(shared_from_this(), _data_dir + "/" + private_key_filename, get_i2p_tunnel_ready_timeout(), _exec));
}

std::unique_ptr<Client> Service::build_client(const std::string& target_id)
{
    return std::unique_ptr<Client>(new Client(shared_from_this(), target_id, get_i2p_tunnel_ready_timeout(), _exec));
}
