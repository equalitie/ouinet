#include <I2PService.h>
#include "i2cp_server.h"

#include <I2CP.h>
#include <Identity.h>
#include <api.h>

#include <fstream>
#include <streambuf>

#include "../../or_throw.h"

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

I2CPServer::I2CPServer(/*std::shared_ptr<Service> service,*/ const string& private_key_filename, uint32_t timeout, const AsioExecutor& exec)
  : //_service(service),
     _exec(exec)
    , _timeout(timeout)
    , _i2p_i2cpserver(new i2p::client::I2CPServer("127.0.0.1",  _c_i2cp_port, true))
{
  // it doesn't even seem that we need a private key as it does not require destination
  //we need a port though but we could start it on the standard i2cp port 7654 then
  //we might add it as a configuration.
  //the problem is that probably we need a fully deplyed i2cp messaging which it
  //seems to take care of it server itself.

  //std::shared_ptr<i2p::client::I2CPServer> _i2p_i2cpserver = make_shared<i2p::client::I2CPServer>("127.0.0.1",  7454, true);
  //i2p::client::I2CPServer*  _i2p_i2cpserver = new ;
  
}


I2CPServer::~I2CPServer()
{
  this->stop_listen();
}

void I2CPServer::start_listen()
{

  _i2p_i2cpserver->Start();
}

void I2CPServer::stop_listen()
{
  _i2p_i2cpserver->Stop();
}
