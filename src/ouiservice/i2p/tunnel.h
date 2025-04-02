#pragma once
#include <boost/asio/spawn.hpp>

#include "../../util/condition_variable.h"

#include "connectionlist.h"

namespace i2p::client {
    class I2PService;
}

namespace ouinet::ouiservice::i2poui {

class Tunnel  {
public:
  AsioExecutor get_executor() {  return _exec; }

  /**
       is called by GenericConnector::is_ready to set a callback when
       the acceptor is ready.

  */
  void wait_to_get_ready(boost::asio::yield_context yield);

  bool has_timed_out() {return _has_timed_out;}

  Tunnel(const AsioExecutor&, std::shared_ptr<i2p::client::I2PService> _i2p_tunnel, uint32_t timeout);

  ~Tunnel();

  void intrusive_add(Connection&);

  asio::ip::tcp::endpoint local_endpoint();

private:
  /**
     Sets the timeout value where the tunnel raise an error
     if it does not get ready by that time

   */
  void set_timeout_to_get_ready(uint32_t timeout);

private:
  AsioExecutor _exec;

  using WorkGuard = asio::executor_work_guard<AsioExecutor>;

  //the tunnel will use this mock work to prevent asio service
  //from exiting while channel is waiting for accepting or connecting
  std::shared_ptr<WorkGuard> _waiting_work;

  // I2PService derives from std::enable_shared_from_this, so it must
  // be a shared_ptr.
  std::shared_ptr<i2p::client::I2PService> _i2p_tunnel;
  ConnectionList _connections;
  std::unique_ptr<ConditionVariable> _ready_condition;
  std::shared_ptr<bool> _was_destroyed;

  bool _has_timed_out = false;

};

} // namespaces
