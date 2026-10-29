#include "socket.h"

namespace ouinet::ouisync_service {

// NOTE: using `std::unreachable` in this code because an instance
// of `OuisyncSocket` can't be created when `WITH_OUISYNC=OFF`.

using endpoint_type = asio_utp::abstract_udp_socket::endpoint_type;
using executor_type = asio_utp::abstract_udp_socket::executor_type;

OuisyncSocket::~OuisyncSocket() {}

/* static */
std::expected<OuisyncSocket, boost::system::error_code>
OuisyncSocket::open(ouisync::Session&, boost::asio::ip::udp, Async) {
    return std::unexpected(asio::error::operation_not_supported);
}

const executor_type& OuisyncSocket::get_executor() {
    std::unreachable();
}

endpoint_type OuisyncSocket::local_endpoint(boost::system::error_code&) const {
    std::unreachable();
}

bool OuisyncSocket::is_open() const {
    std::unreachable();
}

void OuisyncSocket::cancel(boost::system::error_code&) {
    std::unreachable();
}

std::size_t OuisyncSocket::available(boost::system::error_code&) const {
    std::unreachable();
}

void OuisyncSocket::async_receive_from(
    const std::span<boost::asio::mutable_buffer>&,
    endpoint_type&,
    handler
) {
    std::unreachable();
}

// Starts asynchronous datagram send. Invoke the handler on completion, passing it the error code
// and the number of bytes sent.
void OuisyncSocket::async_send_to(
    const std::span<const boost::asio::const_buffer>&,
    const endpoint_type&,
    handler
) {
    std::unreachable();
}

// Send a datagram immediately without blocking. If it can't be done (e.g., the underlying
// send buffer is full), it must return immediately and set the
// `boost::asio::error::would_block` error code.
// Returns the number of bytes sent.
std::size_t OuisyncSocket::immediate_send_to(
    const std::span<const boost::asio::const_buffer>&,
    const endpoint_type&,
    boost::asio::socket_base::message_flags,
    boost::system::error_code&
) {
    std::unreachable();
}

} // namespace
