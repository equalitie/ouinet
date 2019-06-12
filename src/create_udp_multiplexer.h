#pragma once

#include <asio_utp.hpp>
#include <fstream>
#include <boost/filesystem.hpp>
#include "namespaces.h"
#include "logger.h"

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

    if (fs::exists(last_used_port)) {
        fstream file(last_used_port.native());

        if (file.is_open()) {
            uint16_t port = 0;
            file >> port;

            ip::udp::endpoint ep(ip::address_v4::any(), port);

            sys::error_code ec;
            ret.bind(ep, ec);

            if (!ec) {
                LOG_DEBUG("UDP multiplexer bound to last used:", ep);
                return ret;
            }

            LOG_WARN( "Failed to bind UDP multiplexer to last used port:", port
                    , " ec:", ec.message());

        }
        else {
            LOG_WARN( "Failed to open file  ", last_used_port, " "
                    , " to reuse last used UDP port");
        }
    }

    sys::error_code ec;

    ret.bind(ip::udp::endpoint(ip::address_v4::any(), 0), ec);

    assert(!ec);

    LOG_DEBUG("UDP multiplexer bound to:", ret.local_endpoint());

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
