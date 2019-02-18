#include "socks5-client.h"
#include "util.h"
#include "../../or_throw.h"

#include <iostream>

namespace ouinet {
namespace ouiservice {
namespace pt {

/*
 * Communicate connection arguments via Json Parameter Block authentication.
 * This method is described in the PT specification; actual implementation
 * degree is unclear.
 */
/*
ruud:
I can't find any complete documentation on this, and no code that uses it.
Let's implement this once we find a use case for it.

void connection_arguments_json(
    asio::ip::tcp::socket& socket,
    std::map<std::string, std::string>& connection_arguments,
    asio::yield_context yield
) {
}
*/

/*
 * Communicate connection arguments encoded in username/password.
 * This encoding is not described in the specification, but widely implemented.
 */
void connection_arguments_username(
    asio::ip::tcp::socket& socket,
    std::map<std::string, std::string>& connection_arguments,
    asio::yield_context yield
) {
    sys::error_code ec;

    std::string encoded_arguments;
    for (auto i : connection_arguments) {
        if (!encoded_arguments.empty()) {
            encoded_arguments += ";";
        }
        encoded_arguments += string_escape(i.first, ";=");
        encoded_arguments += "=";
        encoded_arguments += string_escape(i.second, ";=");
    }

    std::string packet;
    packet += '\x01'; // authentication scheme version
    packet += (char)(encoded_arguments.size()); // username size
    packet += encoded_arguments; // username
    packet += '\x01'; // password size
    packet += '\x00'; // password

    socket.async_send(
        asio::const_buffers_1(packet.data(), packet.size()),
        0, yield[ec]
    );
    if (ec) {
        return or_throw(yield, ec);
    }

    char reply[2];
    size_t received = socket.async_receive(
        asio::mutable_buffers_1(reply, sizeof(reply)),
        0, yield[ec]
    );
    if (ec) {
        return or_throw(yield, ec);
    }

    if (received != 2) {
        return or_throw(yield, asio::error::connection_reset);
    }
    // authentication scheme version
    if (reply[0] != '\x01') {
        return or_throw(yield, asio::error::access_denied);
    }
    // error code
    if (reply[1] != '\0') {
        return or_throw(yield, asio::error::access_denied);
    }
}


asio::ip::tcp::socket connect_socks5(
    asio::ip::tcp::endpoint proxy_endpoint,
    asio::ip::tcp::endpoint destination_endpoint,
    boost::optional<std::map<std::string, std::string>> connection_arguments,
    asio::io_service& ios,
    asio::yield_context yield,
    Signal<void()>& cancel
) {
    sys::error_code ec;

    asio::ip::tcp::socket socket(ios);

    auto cancel_slot = cancel.connect([&] {
        // tcp::socket::cancel() does not work properly on all platforms
        sys::error_code ec;
        socket.close(ec);
    });

    socket.async_connect(proxy_endpoint, yield[ec]);
    if (ec) {
        return or_throw(yield, ec, std::move(socket));
    }



    std::string negotiation_request;
    negotiation_request += '\x05'; // protocol version 5
    if (connection_arguments) {
        negotiation_request += '\x01'; // 1 authentication methods supported
        //negotiation_request += '\x09'; // Json Parameter Block authentication
        negotiation_request += '\x02'; // username/password authentication
    } else {
        negotiation_request += '\x01'; // 1 authentication methods supported
        negotiation_request += '\x00'; // null authentication
    }

    socket.async_send(
        asio::const_buffers_1(negotiation_request.data(), negotiation_request.size()),
        0, yield[ec]
    );
    if (ec) {
        socket.close();
        return or_throw(yield, ec, std::move(socket));
    }

    size_t received;

    char negotiation_reply[2];
    received = socket.async_receive(
        asio::mutable_buffers_1(negotiation_reply, sizeof(negotiation_reply)),
        0, yield[ec]
    );
    if (ec) {
        socket.close();
        return or_throw(yield, ec, std::move(socket));
    }

    if (received != sizeof(negotiation_reply)) {
        socket.close();
        return or_throw(yield, asio::error::connection_reset, std::move(socket));
    }
    // protocol version 5
    if (negotiation_reply[0] != '\x05') {
        socket.close();
        return or_throw(yield, asio::error::connection_refused, std::move(socket));
    }

    if (connection_arguments) {
        if (negotiation_reply[1] == '\x02') {
            // username/password authentication
            connection_arguments_username(socket, *connection_arguments, yield[ec]);
            if (ec) {
                socket.close();
                return or_throw(yield, ec, std::move(socket));
            }
        } else {
            socket.close();
            return or_throw(yield, asio::error::connection_refused, std::move(socket));
        }
    } else {
        if (negotiation_reply[1] != 0) {
            socket.close();
            return or_throw(yield, asio::error::connection_refused, std::move(socket));
        }
    }



    std::string connect_request;
    connect_request += '\x05'; // protocol version 5
    connect_request += '\x01'; // CONNECT
    connect_request += '\x00'; // reserved
    if (destination_endpoint.address().is_v4()) {
        connect_request += '\x01'; // ipv4 address
        auto address = destination_endpoint.address().to_v4().to_bytes();
        connect_request.append(address.begin(), address.end());
    } else {
        connect_request += '\x04'; // ipv6 address
        auto address = destination_endpoint.address().to_v6().to_bytes();
        connect_request.append(address.begin(), address.end());
    }
    connect_request += (destination_endpoint.port() >> 8) & 0xff;
    connect_request += (destination_endpoint.port() >> 0) & 0xff;

    socket.async_send(
        asio::const_buffers_1(connect_request.data(), connect_request.size()),
        0, yield[ec]
    );
    if (ec) {
        socket.close();
        return or_throw(yield, ec, std::move(socket));
    }

    char connect_reply_start[4];
    received = socket.async_receive(
        asio::mutable_buffers_1(connect_reply_start, sizeof(connect_reply_start)),
        0, yield[ec]
    );
    if (ec) {
        socket.close();
        return or_throw(yield, ec, std::move(socket));
    }
    if (received != sizeof(connect_reply_start)) {
        socket.close();
        return or_throw(yield, asio::error::connection_reset, std::move(socket));
    }
    // protocol version 5
    if (connect_reply_start[0] != '\x05') {
        socket.close();
        return or_throw(yield, asio::error::connection_refused, std::move(socket));
    }
    // error code
    if (connect_reply_start[1] != '\x00') {
        socket.close();
        if (connect_reply_start[1] == '\x02') {
            // connection not allowed by ruleset
            std::cout << "operation not permitted? echt?\n";
            ec = asio::error::no_permission;
        } else if (connect_reply_start[1] == '\x03') {
            // Network unreachable
            ec = asio::error::network_unreachable;
        } else if (connect_reply_start[1] == '\x04') {
            // Host unreachable
            ec = asio::error::host_unreachable;
        } else {
            ec = asio::error::connection_refused;
        }
        return or_throw(yield, ec, std::move(socket));
    }
    // reserved
    if (connect_reply_start[2] != '\x00') {
        socket.close();
        return or_throw(yield, asio::error::connection_refused, std::move(socket));
    }
    // address type
    if (connect_reply_start[3] == '\x01') {
        // ipv4
        char buffer[4];
        received = socket.async_receive(
            asio::mutable_buffers_1(buffer, sizeof(buffer)),
            0, yield[ec]
        );
        if (ec || received != sizeof(buffer)) {
            socket.close();
            return or_throw(yield, asio::error::connection_refused, std::move(socket));
        }
    } else if (connect_reply_start[3] == '\x04') {
        // ipv6
        char buffer[16];
        received = socket.async_receive(
            asio::mutable_buffers_1(buffer, sizeof(buffer)),
            0, yield[ec]
        );
        if (ec || received != sizeof(buffer)) {
            socket.close();
            return or_throw(yield, asio::error::connection_refused, std::move(socket));
        }
    } else if (connect_reply_start[3] == '\x03') {
        // hostname
        char dummy_buffer[256];
        unsigned char size;
        received = socket.async_receive(
            asio::mutable_buffers_1(&size, 1),
            0, yield[ec]
        );
        if (ec || received != 1) {
            socket.close();
            return or_throw(yield, asio::error::connection_refused, std::move(socket));
        }
        received = socket.async_receive(
            asio::mutable_buffers_1(&dummy_buffer, size),
            0, yield[ec]
        );
        if (ec || received != size) {
            socket.close();
            return or_throw(yield, asio::error::connection_refused, std::move(socket));
        }
    } else {
        socket.close();
        return or_throw(yield, asio::error::connection_refused, std::move(socket));
    }
    char port_buffer[2];
    received = socket.async_receive(
        asio::mutable_buffers_1(port_buffer, sizeof(port_buffer)),
        0, yield[ec]
    );
    if (ec || received != sizeof(port_buffer)) {
        socket.close();
        return or_throw(yield, asio::error::connection_refused, std::move(socket));
    }

    // Connection accepted.
    return std::move(socket);
}

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
