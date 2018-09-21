#pragma once

#include <boost/filesystem.hpp>

#include "namespaces.h"

namespace ouinet {

class Client {
private:
    class State;
    class ClientCacheControl;

public:
    Client(asio::io_service& ios);

    ~Client();

    // May throw on error.
    void start(int argc, char* argv[]);

    void stop();

    void set_injector_endpoint(const char*);
    void set_ipns(const char*);
    void set_credentials(const char* injector, const char* cred);

    // Calling this only has meaning after client start.
    boost::filesystem::path get_pid_path() const;
    boost::filesystem::path ca_cert_path() const;

private:
    std::shared_ptr<State> _state;
};

} // ouinet namespace
