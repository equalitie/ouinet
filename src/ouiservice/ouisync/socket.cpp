#include "socket.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/error.hpp>

#include "queue.h"
#include "parse/endpoint.h"
#include "util/log_path.h"
#include "util/str.h"

namespace asio = boost::asio;
namespace ip = boost::asio::ip;
using boost::system::error_code;

namespace ouinet::ouisync_service {

static constexpr size_t outgoing_capacity = 32;
static constexpr size_t incoming_capacity = 32;

static std::optional<ip::udp::endpoint> parse_quic_endpoint(const std::string& s) {
    // PROTO/IP:PORT

    auto i_slash = s.find('/');
    if (i_slash == std::string::npos) {
        return std::nullopt;
    }

    auto proto = s.substr(0, i_slash);
    boost::algorithm::to_lower(proto);

    if (proto != "quic") {
        return std::nullopt;
    }

    return parse::endpoint<ip::udp>(s.substr(i_slash + 1));
}

// Internal state of `OuisyncSocket`. Kept in shared_ptr so that spawned coroutines can safely
// access it even when the socket itself has been moved.
struct OuisyncSocket::State {
    ouisync::NetworkSocket inner;
    asio::ip::udp::endpoint local_endpoint;

    // We are using these intermediate buffers for the following reasons:
    //
    // - The `outgoing` buffer is used to support the `immediate_send_to` operation which must be
    // non-async and non-blocking. The operation pushed the data into this buffer if it's not full
    // or returns `would_block` if it is. This is needed because `ouisync::NetworkSocket` doesn't
    // have any non-async send operation. A possible alternative would be to invoke
    // `ouisync::NetworkSocket` with `detached` completion token. To make it more robust, some sort
    // of concurrency limit should be implemented too.
    //
    // - The `incoming` buffer is needed to implement the `available` method which is done by
    // counting the total number of bytes across all the messages in the `incoming` buffer. A
    // possible alternative to this would be to always return 0 from the method but it would need to
    // be tested to make sure it doesn't affect performance too badly.
    //
    // - Both buffers are also useful to implement the `cancel` method as `ouisync::NetworkSocket`
    // doesn't support per-object cancellation. An alternative would be to keep a collection of
    // cancellation tokens for every ongoing async operation and trigger them when `cancel` is
    // called.
    detail::Queue outgoing;
    detail::Queue incoming;

    bool open = true;

    State(
        const asio::any_io_executor& exec,
        ouisync::NetworkSocket inner,
        endpoint_type local_endpoint
    ) :
        inner(std::move(inner)),
        local_endpoint(std::move(local_endpoint)),
        outgoing(exec, outgoing_capacity),
        incoming(exec, incoming_capacity)
    {}

    const asio::any_io_executor& get_executor() {
        return outgoing.get_executor();
    }

    void recv_loop(Async yield) {
        constexpr size_t max_datagram_size = 4096;

        while (open) {
            auto recv = inner.recv_from(max_datagram_size, yield);
            if (!recv) {
                std::ignore = incoming.async_push(recv.error(), {}, yield);
                break;
            }

            auto ep = parse::endpoint<ip::udp>(recv->addr);
            if (!ep) {
                break;
            }

            auto send = incoming.async_push(error_code(), { *ep, std::move(recv->data) }, yield);
            if (!send) {
                break;
            }
        }
    }

    void send_loop(Async yield) {
        while (open) {
            auto recv = outgoing.async_pop(yield);
            if (!recv) {
                break;
            }

            auto [ ep, data ] = std::move(recv.value());
            auto send = inner.send_to(data, util::str(ep), yield);
            if (!send) {
                break;
            }
        }
    }

    void close(Async yield) {
        open = false;
        incoming.cancel();
        outgoing.cancel();

        // Flush outgoing messages
        while (true) {
            auto recv = outgoing.try_pop();
            if (!recv) {
                break;
            }

            auto [ ec, ep, data ] = std::move(*recv);
            if (ec) {
                // this should not happen in practice because we don't push errors to the outgoing
                // queue.
                break;
            }

            auto send = inner.send_to(data, util::str(ep), yield);
            if (!send) {
                break;
            }
        }

        std::ignore = inner.close(yield);
    }
};

OuisyncSocket::OuisyncSocket(std::shared_ptr<State> state, util::LogPath log_path)
    : _state(std::move(state))
{
    task::spawn_detached(
        _state->get_executor(),
        [state = _state, log_path] (asio::yield_context y) {
            state->send_loop(Async(y, log_path));
        }
    );

    task::spawn_detached(
        _state->get_executor(),
        [state = _state, log_path] (asio::yield_context y) {
            state->recv_loop(Async(y, log_path));
        }
    );
}

OuisyncSocket::~OuisyncSocket() {
    if (!_state) {
        return;
    }

    // Close in the background, ignoring errors
    task::spawn_detached(
        get_executor(),
        [state = std::move(_state)] (asio::yield_context yield) mutable {
            state->close(Async(yield));
        }
    );
}

std::expected<OuisyncSocket, boost::system::error_code>
OuisyncSocket::open(ouisync::Session& session, ip::udp proto, Async yield) {
    auto inner_e = proto == ip::udp::v4() ?
        session.open_network_socket_v4(yield) :
        session.open_network_socket_v6(yield);
    if (!inner_e) {
        return std::unexpected(inner_e.error());
    }
    auto inner = std::move(inner_e.value());

    auto endpoint_strs_e = session.get_local_listener_addrs(yield);
    if (!endpoint_strs_e) {
        return std::unexpected(endpoint_strs_e.error());
    }
    auto endpoint_strs = std::move(endpoint_strs_e.value());

    std::optional<ip::udp::endpoint> local_endpoint;
    for (auto s : endpoint_strs) {
        auto endpoint = parse_quic_endpoint(s);
        if (!endpoint) continue;

        if (proto == ip::udp::v4() && endpoint->address().is_v4() ||
            proto == ip::udp::v6() && endpoint->address().is_v6())
        {
            local_endpoint = endpoint.value();
            break;
        }
    }

    if (!local_endpoint) {
        return std::unexpected(asio::error::no_protocol_option);
    }

    auto exec = inner.get_executor();

    return OuisyncSocket(
        std::make_shared<State>(
            exec,
            std::move(inner),
            *local_endpoint
        ),
        yield.log_path()
    );
}

const OuisyncSocket::executor_type& OuisyncSocket::get_executor() {
    if (!_state) {
        throw boost::system::system_error(asio::error::shut_down);
    }

    return _state->get_executor();
}

OuisyncSocket::endpoint_type OuisyncSocket::local_endpoint(error_code& ec) const {
    if (!_state) {
        ec = asio::error::shut_down;
        return {};
    }

    ec = error_code();
    return _state->local_endpoint;
}

bool OuisyncSocket::is_open() const {
    return _state && _state->inner;
}

void OuisyncSocket::cancel(error_code& ec) {
    if (!_state) {
        ec = asio::error::shut_down;
        return;
    }

    _state->incoming.cancel();
    _state->outgoing.cancel();
    ec = error_code();
}

std::size_t OuisyncSocket::available(error_code& ec) const {
    if (!_state) {
        ec = asio::error::shut_down;
        return 0;
    }

    return _state->incoming.bytes();
}

void OuisyncSocket::async_receive_from(
    const std::span<asio::mutable_buffer>& buffers,
    endpoint_type& sender,
    handler handler
) {
    if (!_state) {
        handler(asio::error::shut_down, 0);
        return;
    }

    auto cancellation_slot = handler.get_cancellation_slot();

    _state->incoming.async_pop(
        asio::bind_cancellation_slot(
            std::move(cancellation_slot),
            [buffers, &sender, handler = std::move(handler)]
            (error_code ec, std::tuple<endpoint_type, std::vector<uint8_t>> payload) mutable {
                auto [ payload_ep, payload_data ] = std::move(payload);

                if (ec) {
                    handler(ec, 0);
                } else {
                    asio::buffer_copy(buffers, asio::buffer(payload_data));
                    sender = payload_ep;
                    handler(ec, payload_data.size());
                }
            }
        )
    );
}

void OuisyncSocket::async_send_to(
    const std::span<const asio::const_buffer>& buffers,
    const endpoint_type& receiver,
    handler handler
) {
    if (!_state) {
        handler(asio::error::shut_down, 0);
        return;
    }

    auto size = asio::buffer_size(buffers);
    std::vector<uint8_t> data(size);
    asio::buffer_copy(asio::buffer(data), buffers);

    auto cancellation_slot = handler.get_cancellation_slot();

    _state->outgoing.async_push(
        error_code(),
        { receiver, std::move(data) },
        asio::bind_cancellation_slot(
            std::move(cancellation_slot),
            [size, handler = std::move(handler)] (error_code ec) mutable {
                if (ec) {
                    handler(ec, 0);
                } else {
                    handler(ec, size);
                }
            }
        )
    );
}

std::size_t OuisyncSocket::immediate_send_to(
    const std::span<const asio::const_buffer>& buffers,
    const endpoint_type& receiver,
    asio::socket_base::message_flags flags,
    error_code& ec
) {
    std::ignore = flags;

    if (!_state) {
        ec = asio::error::shut_down;
        return 0;
    }

    if (_state->outgoing.full()) {
        ec = asio::error::would_block;
        return 0;
    }

    auto size = asio::buffer_size(buffers);
    std::vector<uint8_t> data(size);
    asio::buffer_copy(asio::buffer(data), buffers);

    bool pushed = _state->outgoing.try_push(error_code(), { receiver, std::move(data) });
    assert(pushed);

    ec = error_code();

    return size;
}

} // namespace ouinet::ouisync_service
