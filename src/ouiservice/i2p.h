#pragma once

#ifdef __EXPERIMENTAL__

#include "../ouiservice.h"
#include "i2p/direct/client.h"
#include "i2p/direct/server.h"
#include "i2p/direct/service.h"
#include "i2pd/libi2pd/Destination.h"

namespace ouinet {
    //using I2pServer = i2p_direct::Server;
    //using I2pClient = i2p_direct::Client;
    //using I2pService = i2p_direct::Service;
    //using I2pClientDestination = i2p::client::ClientDestination;
}

#else // ifdef __EXPERIMENTAL__
 
#include "i2p/fwd.h"

# endif
