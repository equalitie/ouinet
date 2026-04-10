#pragma once

// Forward declarations

namespace i2p::client {
    class ClientDestination;
}

namespace ouinet {
    namespace ouiservice::i2poui {
        class Server;
        class Client;
        class Service;
    }

    using I2pServer = ouiservice::i2poui::Server;
    using I2pClient = ouiservice::i2poui::Client;
    using I2pService = ouiservice::i2poui::Service;
    using I2pClientDestination = i2p::client::ClientDestination;
}
