#pragma once

#include <asio_utp.hpp>
#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include "namespaces.h"
#include "logger.h"
#include "constants.h"

namespace ouinet {

/*
 * Create a new UDP multiplexer. Try to reuse the endpoint from last app run if
 * possible. If not, pick a random port and store it in a file so it can be
 * reused later.
 */
static
asio_utp::udp_multiplexer
create_udp_multiplexer( asio::io_service& ios
                      , fs::path last_used_port_path
                      , const boost::optional<uint16_t>& settings_port = boost::none)
{
    using namespace std;
    namespace ip = asio::ip;

    asio_utp::udp_multiplexer ret(ios);
    struct PortBinding {
        string attempt_type;
        uint16_t port;
        PortBinding(string type, uint16_t port) : attempt_type(std::move(type)), port(port){};
    };
    list<PortBinding> port_binding_attempts{};


    auto read_last_used_port_or_use_random = [&last_used_port_path] () {
        uint16_t port = random_port_selection;

        if (fs::exists(last_used_port_path)) {
            fstream file(last_used_port_path.string());

            if (file.is_open()) {
                file >> port;
            }
            else {
                LOG_WARN( "Failed to open file ", last_used_port_path, " "
                        , " to reuse last used UDP port");
            }
        }
        return port;
    };

    auto bind = [] ( asio_utp::udp_multiplexer& m
                   , const PortBinding& port_bind
                   , sys::error_code& ec) {
        m.bind(ip::udp::endpoint(ip::address_v4::any(), port_bind.port), ec);

        if (!ec) {
            LOG_INFO( "UDP multiplexer bound to ", port_bind.attempt_type
                     , " port: ", m.local_endpoint().port());
        } else {
            LOG_WARN( "Failed to bind UDP multiplexer to ", port_bind.attempt_type
                     , " port: ", port_bind.port, "; ec=", ec);
        }
    };

    auto write_last_used_port = [&last_used_port_path] (uint16_t port) {
        fstream file(last_used_port_path.string()
                    , fstream::binary | fstream::trunc | fstream::out);

        if (file.is_open()) {
            file << port;
        } else {
            LOG_WARN( "Failed to store UDP multiplexer port to file "
                    , last_used_port_path, " for later reuse");
        }
    };

    // Use the port defined in `udp-mux-port` via ouinet.conf or CLI options
    if (settings_port) {
        port_binding_attempts.emplace_back("settings", *settings_port);
    }
    // Use previous port, if saved, or pick a random one.
    auto last_or_random = read_last_used_port_or_use_random();
    if (last_or_random != random_port_selection) {
        port_binding_attempts.emplace_back("last used", last_or_random);
    } else {
        port_binding_attempts.emplace_back("random", last_or_random);
    }
    // Fallback to default port.
    port_binding_attempts.emplace_back("default", default_udp_port);
    // Last resort, try again to set a random port.
    port_binding_attempts.emplace_back("last resort", random_port_selection);

    sys::error_code ec;
    for (const auto& port_binding_attempt: port_binding_attempts) {
        ec.clear();
        bind(ret, port_binding_attempt, ec);
        if (!ec) {
            write_last_used_port(ret.local_endpoint().port());
            return ret;
        }
    }
    assert(!ec);
    return ret;
}

} // ouinet namespace