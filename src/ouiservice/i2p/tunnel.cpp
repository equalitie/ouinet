#include <I2PService.h>
#include <I2PTunnel.h>

#include "../../logger.h"
#include "../../defer.h"

#include "tunnel.h"

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

Tunnel::Tunnel(const AsioExecutor& exec, std::shared_ptr<i2p::client::I2PService> i2p_tunnel, uint32_t timeout)
  : _exec(exec), _i2p_tunnel(std::move(i2p_tunnel)),
    _was_destroyed(make_shared<bool>(false))
{
  // I2Pd doesn't implicitly keep executor busy, so we need to
  // do it ourselves.
  _waiting_work = std::make_shared<WorkGuard>(_exec);
  _i2p_tunnel->Start();
  set_timeout_to_get_ready(timeout);
};

/**
   Sets the timeout value where the tunnel raise an error
   if it does not get ready by that time
     
*/
void Tunnel::set_timeout_to_get_ready(uint32_t timeout)
{
  _i2p_tunnel->SetConnectTimeout(timeout);
}

/**
   is called by GenericConnector::is_ready to set a callback when
   the acceptor is ready.

*/
void Tunnel::wait_to_get_ready(boost::asio::yield_context yield) {
    
  auto wd = _was_destroyed;

  sys::error_code ec;

  assert(!_ready_condition);
  _ready_condition = make_unique<ConditionVariable>(_exec);
  auto on_exit = defer([this, wd] { if (!*wd) _ready_condition = nullptr; });

  // Wait till we find a route to the service and tunnel is ready then try to
  // acutally connect and then unblock
  OUI_LOG_DEBUG("Waiting for I2P tunnel to get established");
  
  auto exec = _exec;

  _i2p_tunnel->AddReadyCallback([&exec, wd, &ec, this](const sys::error_code& error) mutable {
      // _i2p_tunnel is not using our executor and thus will execute the
      // callback in a different thread, so use `asio::post` to get back to our
      // thread.
      asio::post(exec, [wd = std::move(wd), &ec, this, error] () {
          ec = error;
          if (*wd) return;
          _ready_condition->notify();
      });
    });

  // This _returns_ once the `block` thing create above gets destroyed
  // i.e. when the handler finishes.
  _ready_condition->wait(yield);

  if (ec == boost::asio::error::timed_out) {
    OUI_LOG_ERROR("I2P Tunnel failed to be established in timely manner, trying again");
    _has_timed_out = true;
  }
  
  if (!*wd) {
      OUI_LOG_DEBUG("I2P Tunnel has been established");  // used by integration tests
  }
  else {
      return or_throw(yield, asio::error::operation_aborted);
  }
}

boost::asio::ip::tcp::endpoint Tunnel::local_endpoint() {
  //The client_tunnel can't return its port becaues it doesn't know
  //that it is a client i2p tunnel, all it knows is that it is an
  //i2ptunnel holding some connections but doesn't know how connections
  //are created.
  return dynamic_cast<i2p::client::I2PClientTunnel*>(_i2p_tunnel.get())->GetLocalEndpoint();
}

void Tunnel::intrusive_add(Connection& connection) {
    _connections.add(connection);
}

Tunnel::~Tunnel() {
  *_was_destroyed = true;
  _connections.close_all();
  _i2p_tunnel->Stop();

  if (_ready_condition) {
      _ready_condition->notify();
  }
}
