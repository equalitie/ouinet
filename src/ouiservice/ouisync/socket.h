#pragma once

#include <asio_utp/udp_socket.hpp>
#include <expected>
#include <ouisync.hpp>

#include "util/async.h"

namespace ouinet::ouisync_service {

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

    // Send a datagram immediatelly without blocking. If it can't be done (e.g., the underlying
    // send buffer is full), it must return immediatelly and set the
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

    OuisyncSocket(std::shared_ptr<State>);
};

} // namespace ouinet::ouisync_service
