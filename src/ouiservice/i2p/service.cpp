#include "service.h"

//i2p stuff
#include <I2PTunnel.h>
#include <Log.h>
#include <api.h>

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

Service::Service(const string& datadir, boost::asio::io_service& ios)
    : _ios(ios)
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
}

Service::Service(Service&& other)
    : _ios(other._ios)
    , _data_dir(std::move(other._data_dir))
{}

Service& Service::operator=(Service&& other)
{
    assert(&_ios == &other._ios);
    _data_dir = std::move(other._data_dir);
    return *this;
}

Service::~Service()
{
    i2p::api::StopI2P();
}

std::unique_ptr<Server> Service::build_server(const std::string& private_key_filename)
{
    return std::unique_ptr<Server>(new Server(shared_from_this(), _data_dir + "/" + private_key_filename, get_i2p_tunnel_ready_timeout(), _ios));
}

std::unique_ptr<Client> Service::build_client(const std::string& target_id)
{
    return std::unique_ptr<Client>(new Client(shared_from_this(), target_id, get_i2p_tunnel_ready_timeout(), _ios));
}
