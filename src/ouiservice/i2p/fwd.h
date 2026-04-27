#pragma once

// Forward declarations

namespace i2p::client {
    class ClientDestination;
}

namespace ouinet {
    namespace i2p_direct {
        class Server;
        class Client;
        class Service;
        using I2pClientDestination = i2p::client::ClientDestination;
    }

    using I2pServer = i2p_direct::Server;
    using I2pClient = i2p_direct::Client;
    using I2pService = i2p_direct::Service;
    using I2pClientDestination = i2p::client::ClientDestination;
}
