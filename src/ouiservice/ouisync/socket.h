#pragma once

#include <asio_utp/udp_socket.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <expected>
#include <ouisync.hpp>

#include "util/async.h"
#include "util/trace.h"

namespace ouinet::ouisync_service {

static constexpr size_t outgoing_buffer_size = 1024 * 1024;
static constexpr size_t incoming_buffer_size = 1024 * 1024;

// UDP socket backed by ouisync.
class OuisyncSocket : public asio_utp::abstract_udp_socket {
public:
    OuisyncSocket(OuisyncSocket&&) = default;
    OuisyncSocket& operator = (OuisyncSocket&&) = default;

    OuisyncSocket(const OuisyncSocket&) = delete;
    OuisyncSocket& operator = (const OuisyncSocket&) = delete;

    ~OuisyncSocket() override;

    // Opens Ouisync-backed UDP socket bound to an interface with the given IP version (udp::v4 or
    // udp::v6)
    static std::expected<OuisyncSocket, boost::system::error_code>
    open(ouisync::Session&, boost::asio::ip::udp, Async);

    const executor_type& get_executor() override;

    endpoint_type local_endpoint(boost::system::error_code&) const override;

    // DEBUG
    endpoint_type local_endpoint() const {
        boost::system::error_code ec;
        return local_endpoint(ec);
    }

    bool is_open() const override;

    void cancel(boost::system::error_code&) override;

    std::size_t available(boost::system::error_code&) const override;

    void async_receive_from(
        const std::span<boost::asio::mutable_buffer>&,
        endpoint_type&,
        handler
    ) override;

    // Starts asynchronous datagram send. Invoke the handler on completion, passing it the error code
    // and the number of bytes sent.
    void async_send_to(
        const std::span<const boost::asio::const_buffer>&,
        const endpoint_type&,
        handler
    ) override;

    // Send a datagram immediately without blocking. If it can't be done (e.g., the underlying
    // send buffer is full), it must return immediately and set the
    // `boost::asio::error::would_block` error code.
    // Returns the number of bytes sent.
    std::size_t immediate_send_to(
        const std::span<const boost::asio::const_buffer>&,
        const endpoint_type&,
        boost::asio::socket_base::message_flags,
        boost::system::error_code&
    ) override;

private:
    struct State;
    std::shared_ptr<State> _state;

    OuisyncSocket(std::shared_ptr<State>, Trace);
};

} // namespace ouinet::ouisync_service
