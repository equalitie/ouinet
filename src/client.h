#pragma once

#include <boost/filesystem.hpp>

#include "namespaces.h"

namespace ouinet {

class ClientConfig;

class Client {
private:
    class State;
    class ClientCacheControl;

public:
    static boost::filesystem::path get_or_gen_ca_root_cert(const std::string repo_root);

    Client(asio::io_service& ios, ClientConfig);

    ~Client();

    void start();
    void stop();

    void set_injector_endpoint(const char*);
    void set_credentials(const char* injector, const char* cred);

    void charging_state_change(bool is_charging);
    void wifi_state_change(bool is_wifi_connected);

    // Calling this only has meaning after client start.
    boost::filesystem::path get_pid_path() const;
    boost::filesystem::path ca_cert_path() const;

private:
    std::shared_ptr<State> _state;
};

} // ouinet namespace
