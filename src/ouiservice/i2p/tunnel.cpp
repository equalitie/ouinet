#include "tunnel.h"

#include <I2PService.h>

#include "../../blocker.h"


#include "../../logger.h"
#include "../../defer.h"
#include "../../or_throw.h"

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

Tunnel::Tunnel(boost::asio::io_service& ios, std::shared_ptr<i2p::client::I2PService> i2p_tunnel, uint32_t timeout)
  : _ios(ios), _i2p_tunnel(std::move(i2p_tunnel)),
    _was_destroyed(make_shared<bool>(false))
{
  // I2Pd doesn't implicitly keep io_service busy, so we need to
  // do it ourselves.
  _waiting_work = std::make_shared<boost::asio::io_service::work>(_ios);
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
  _ready_condition = make_unique<ConditionVariable>(_ios);
  auto on_exit = Defer{[this, wd] { if (!*wd) _ready_condition = nullptr; }};

    // Wait till we find a route to the service and tunnel is ready then try to
    // acutally connect and then unblock
  LOG_DEBUG("Waiting for I2P tunnel to get established");
  
  _i2p_tunnel->AddReadyCallback([wd, &ec, this](const sys::error_code& error) mutable {
      ec = error;
      if (*wd) return;
      _ready_condition->notify();
    });

  // This _returns_ once the `block` thing create above gets destroyed
  // i.e. when the handler finishes.
  _ready_condition->wait(yield);

  if (!*wd) {
      LOG_DEBUG("I2P Tunnel has been established");
  }
  else {
      return or_throw(yield, asio::error::operation_aborted);
  }
}

Tunnel::~Tunnel() {
  *_was_destroyed = true;
  _connections.close_all();
  _i2p_tunnel->Stop();

  if (_ready_condition) {
      _ready_condition->notify();
  }
}
