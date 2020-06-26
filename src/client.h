#pragma once

#include <boost/filesystem.hpp>

#include "constants.h"

#include "namespaces.h"

namespace ouinet {

namespace http_ {

// This indicates to the client that the agent considers
// this request and its associated response to be part of a group
// whose identifier is the value of this header.
// This may be used by the client to affect its processing,
// e.g. announcement or storage.
static const std::string request_group_hdr = header_prefix + "Group";

// The presence of this HTTP request header with the true value below
// instructs the client to avoid request mechanisms that
// would reveal the request or the associated response to other users.
static const std::string request_private_hdr = header_prefix + "Private";
static const std::string request_private_true = "true";  // case insensitive

}

class ClientConfig;

class Client {
private:
    class State;
    class ClientCacheControl;

public:
    static boost::filesystem::path get_or_gen_ca_root_cert(const std::string repo_root);

    Client(asio::io_context&, ClientConfig);

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
