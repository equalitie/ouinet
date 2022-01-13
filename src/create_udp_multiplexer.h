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
                      , fs::path last_used_port)
{
    using namespace std;
    namespace ip = asio::ip;

    asio_utp::udp_multiplexer ret(ios);

    auto bind = [] ( asio_utp::udp_multiplexer& m
                   , uint16_t port
                   , sys::error_code& ec) {
        m.bind(ip::udp::endpoint(ip::address_v4::any(), port), ec);
    };

    if (fs::exists(last_used_port)) {
        fstream file(last_used_port.native());

        if (file.is_open()) {
            uint16_t port = 0;
            file >> port;

            sys::error_code ec;
            bind(ret, port, ec);

            if (!ec) {
                LOG_INFO("UDP multiplexer bound to last used port: ", port);
                return ret;
            }

            // TODO: Move code to implementation file, use `util/quote_error_message.h`.
            LOG_WARN( "Failed to bind UDP multiplexer to last used port: ", port
                    , "; ec=", ec);

        }
        else {
            LOG_WARN( "Failed to open file ", last_used_port, " "
                    , " to reuse last used UDP port");
        }
    }

    sys::error_code ec;

    bind(ret, default_udp_port, ec);

    if (ec) {
        LOG_WARN("Failed to bind to the default UDP port ", default_udp_port
                , " picking another port at random");
        ec = {};
        bind(ret, 0, ec);
        assert(!ec);
    }

    LOG_DEBUG("UDP multiplexer bound to: ", ret.local_endpoint());

    fstream file( last_used_port.native()
                , fstream::binary | fstream::trunc | fstream::out);

    if (file.is_open()) {
        file << ret.local_endpoint().port();
    } else {
        LOG_WARN("Failed to store UDP multiplexer port to file "
                , last_used_port, " for later reuse");
    }

    return ret;
}

}
