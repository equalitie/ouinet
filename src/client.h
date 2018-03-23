#pragma once

#include <boost/filesystem.hpp>

#include "namespaces.h"

namespace ouinet {

class Client {
private:
    class State;

public:
    Client(asio::io_service& ios);

    ~Client();

    // May throw on error.
    void start(int argc, char* argv[]);

    void stop();

    void set_injector_endpoint(const char*);
    void set_ipns(const char*);

    boost::filesystem::path get_pid_path() const;

private:
    std::shared_ptr<State> _state;
};

} // ouinet namespace
