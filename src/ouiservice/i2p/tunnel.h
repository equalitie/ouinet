#pragma once
#include <boost/asio/spawn.hpp>

#include "../../util/condition_variable.h"

#include "connectionlist.h"

namespace i2p { namespace client {
    class I2PClientTunnel;
    class I2PService;
}}

namespace ouinet {
namespace ouiservice {
namespace i2poui {

  class Tunnel  {
public:
  boost::asio::io_service& get_io_service() {  return _ios; }

  /**
     Sets the timeout value where the tunnel raise an error
     if it does not get ready by that time
     
   */
  void set_timeout_to_get_ready(uint32_t timeout);
  
  /**
       is called by GenericConnector::is_ready to set a callback when
       the acceptor is ready.

  */
  void wait_to_get_ready(boost::asio::yield_context yield);
  
  Tunnel(boost::asio::io_service& ios, std::shared_ptr<i2p::client::I2PService> _i2p_tunnel, uint32_t timeout);
  
  ~Tunnel();
  
 private:
  friend class Client;
  friend class Server;

  boost::asio::io_service& _ios;

  //the tunnel will use this mock work to prevent asio service
  //from exiting while channel is waiting for accepting or connecting
  std::shared_ptr<boost::asio::io_service::work> _waiting_work;

  // I2PService derives from std::enable_shared_from_this, so it must
  // be a shared_ptr.
  std::shared_ptr<i2p::client::I2PService> _i2p_tunnel;
  ConnectionList _connections;
  std::unique_ptr<ConditionVariable> _ready_condition;
  std::shared_ptr<bool> _was_destroyed;

};

} // i2poui namespace
}
}
