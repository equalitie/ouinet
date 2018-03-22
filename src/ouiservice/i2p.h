#pragma once

#include "../ouiservice.h"
#include "i2p/client.h"
#include "i2p/server.h"
#include "i2p/service.h"

namespace ouinet {
namespace ouiservice {

using I2pOuiServiceServer = i2poui::Server;
using I2pOuiServiceClient = i2poui::Client;
using I2pOuiService = i2poui::Service;

} // ouiservice namespace
} // ouinet namespace
