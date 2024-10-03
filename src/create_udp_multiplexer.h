#pragma once

#include <asio_utp.hpp>
#include <fstream>
#include <boost/filesystem.hpp>
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
                      , fs::path last_used_port_path)
{
    using namespace std;
    namespace ip = asio::ip;

    asio_utp::udp_multiplexer ret(ios);

    auto read_last_used_port_or_use_random = [&last_used_port_path] () {
        uint16_t port = random_port_selection;

        if (fs::exists(last_used_port_path)) {
            fstream file(last_used_port_path.string());

            if (file.is_open()) {
                file >> port;
            }
            else {
                LOG_WARN("Failed to open file ", last_used_port_path, " "
                        , " to reuse last used UDP port");
            }
        }
        return port;
    };

    auto bind = [] ( asio_utp::udp_multiplexer& m
                   , uint16_t port
                   , sys::error_code& ec) {
        m.bind(ip::udp::endpoint(ip::address_v4::any(), port), ec);

        if (!ec) {
            LOG_INFO("UDP multiplexer bound to port: ", m.local_endpoint().port());
        } else {
            LOG_WARN( "Failed to bind UDP multiplexer to port: ", port,
                      "; ec=", ec);
        }
    };

    auto write_last_used_port = [&last_used_port_path] (uint16_t port) {
        fstream file(last_used_port_path.string()
                    , fstream::binary | fstream::trunc | fstream::out);

        if (file.is_open()) {
            file << port;
        } else {
            LOG_WARN("Failed to store UDP multiplexer port to file "
            , last_used_port_path, " for later reuse");
        }
    };

    std::list<uint16_t> port_options{};
    port_options.push_back(read_last_used_port_or_use_random()); // Use previous port, if saved, or pick a random one.
    port_options.push_back(default_udp_port); // Fallback to default port.
    port_options.push_back(random_port_selection); // Last resort, try again to set a random port.

    sys::error_code ec;
    for (const auto& port: port_options) {
        ec.clear();
        bind(ret, port, ec);
        if (!ec) {
            write_last_used_port(ret.local_endpoint().port());
            return ret;
        }
    }
    assert(!ec);
    return ret;
}

} // ouinet namespace