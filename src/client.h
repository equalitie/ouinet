#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/filesystem.hpp>

#include "declspec.h"
#include "constants.h"

#include "namespaces.h"
#include "client_config.h"
#include "bittorrent/mock_dht.h"
#include "util/log_path.h"

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

namespace bittorrent {
    class DhtBase;
}

class ClientConfig;

class OUINET_DECL Client {
private:
    class State;
    class ClientCacheControl;
    using MockDhtBuilder = std::function<std::shared_ptr<bittorrent::MockDht> ()>;

public:
    enum class RunningState {
        Created,  // not told to start yet (initial)
        Failed,  // told to start, error precludes from continuing (final)
        Starting,  // told to start, some operations still pending completion
        Degraded,  // told to start, some operations succeeded but others failed
        Started,  // told to start, all operations succeeded
        Stopping,  // told to stop, some operations still pending completion
        Stopped,  // told to stop, all operations succeeded (final)
    };

    static boost::filesystem::path get_or_gen_ca_root_cert(const std::string repo_root);

    Client(
        asio::io_context&,
        ClientConfig,
        // For use in tests
        util::LogPath log_path = {},
        std::optional<MockDhtBuilder> dht_builder = {});

    ~Client();

    void start();
    void stop();
    RunningState get_state() const noexcept;
    asio::ip::tcp::endpoint get_proxy_endpoint() const noexcept;
    std::string get_frontend_endpoint() const noexcept;
    AsioExecutor get_executor() const noexcept;

    void charging_state_change(bool is_charging);
    void wifi_state_change(bool is_wifi_connected);

    // Calling this only has meaning after client start.
    boost::filesystem::path get_pid_path() const;
    boost::filesystem::path ca_cert_path() const;

    ClientConfig const& config() const;

    std::shared_ptr<bittorrent::DhtBase> get_dht() const;

private:
    std::shared_ptr<State> _state;
};

inline std::ostream& operator<<(std::ostream& os, Client::RunningState state) {
    using S = Client::RunningState;
    switch (state) {
        case S::Created: return os << "Created";
        case S::Failed: return os << "Failed";
        case S::Starting: return os << "Starting";
        case S::Degraded: return os << "Degraded";
        case S::Started: return os << "Started";
        case S::Stopping: return os << "Stopping";
        case S::Stopped: return os << "Stopped";
        default: return os << "???";
    }
}

} // ouinet namespace
