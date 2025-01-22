#pragma once

#include "../../ouiservice.h"

#include "tunnel.h"
#include <bits/stdint-uintn.h>

namespace i2p { namespace data {
    class PrivateKeys;
}}

namespace i2p { namespace client {
    class I2CPServer;
}
}
namespace ouinet {
namespace ouiservice {
namespace i2poui {

class Service;

class I2CPServer {
private:
    // Client is constructed by i2poui::Service
    friend class Service;

  I2CPServer( /*std::shared_ptr<Service> service
          ,*/
	  const std::string& private_key_filename
            , uint32_t timeout, const AsioExecutor&);

   public:
    ~I2CPServer();

    void start_listen();
    void stop_listen();


private:
  //std::shared_ptr<Service> _service;
    AsioExecutor _exec;
    uint32_t _timeout;
  std::shared_ptr<i2p::client::I2CPServer> _i2p_i2cpserver;

  static const uint16_t _c_i2cp_port = 7454;

};

} // i2poui namespace
} // ouiservice namespace
} // ouinet namespace
