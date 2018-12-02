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
    
    bool has_timed_out() {return _has_timed_out;}
    
    Tunnel(boost::asio::io_service& ios, std::shared_ptr<i2p::client::I2PService> _i2p_tunnel, uint32_t timeout);
    
    ~Tunnel();
    
    /**
     * uses the control connection to send handshake 
     * wait for the reply.
     * 
     * return true if it is successful or false otherwise
     */
    bool do_handshake(boost::asio::yield_context yield);
    
    //access functions
    bool is_ready() { return ready; }
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
    
    // variables controlling the health of the tunnel
    //Connection control_connection;
    uint32_t _port = 0;
    
    bool sent_handshake = false;
    bool received_handshake = false;
    bool ready = false;
    
    //tunnel id sent over handshake
    uint32_t id;
    
    //speed of the tunnel in byte per second
    double speed;
    
    //the hash of test blob sends on ping test
    uint32_t speed_test_hash;
    
    bool _has_timed_out = false;
    
};
    
} // i2poui namespace
}
}
