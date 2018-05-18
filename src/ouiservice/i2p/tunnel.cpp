#include "tunnel.h"

#include <I2PService.h>

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

Tunnel::Tunnel(boost::asio::io_service& ios, std::unique_ptr<i2p::client::I2PService> _i2p_tunnel, uint32_t timeout)
  : _ios(ios), _i2p_tunnel(std::move(_i2p_tunnel))
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
    

    sys::error_code ec;
    ConditionVariable ready_condition(_ios);

    // Wait till we find a route to the service and tunnel is ready then try to
    // acutally connect and then unblock
    _i2p_tunnel->AddReadyCallback([&ec, &ready_condition](const sys::error_code& error) mutable {
        ec = error;
        ready_condition.notify();
    });

    // This _returns_ once the `block` thing create above gets destroyed
    // i.e. when the handler finishes.
    ready_condition.wait(yield);

  }


Tunnel::~Tunnel() {
  _connections.close_all();
  _i2p_tunnel->Stop();

}
