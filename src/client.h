#pragma once

#include "namespaces.h"

namespace ouinet {

class Client {
private:
    class State;

public:
    Client(asio::io_service& ios);
    ~Client();

    bool start(int argc, char* argv[]);
    void stop();

private:
    std::unique_ptr<State> _state;
};

} // ouinet namespace
