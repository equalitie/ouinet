#include "socket.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/error.hpp>

#include "ouiservice/ouisync/buffer.h"
#include "parse/endpoint.h"
#include "util/str.h"
#include "util/trace.h"

namespace asio = boost::asio;
namespace ip = boost::asio::ip;
using boost::system::error_code;

namespace ouinet::ouisync_service {

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
    // non-async and non-blocking. The operation pushes the data into this buffer if it's not full
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
    detail::AsyncDatagramBuffer outgoing;
    detail::AsyncDatagramBuffer incoming;

    State(
        const asio::any_io_executor& ex,
        ouisync::NetworkSocket inner,
        endpoint_type local_endpoint
    ) :
        inner(std::move(inner)),
        local_endpoint(std::move(local_endpoint)),
        outgoing(ex, outgoing_buffer_size),
        incoming(ex, incoming_buffer_size)
    {}

    const asio::any_io_executor& get_executor() {
        return outgoing.get_executor();
    }

    void recv_loop(Async yield) {
        constexpr size_t max_datagram_size = 4096;

        while (true) {
            auto recv = inner.recv_from(max_datagram_size, yield);
            if (!recv) {
                // TODO: propagate the error?
                break;
            }

            auto ep = parse::endpoint<ip::udp>(recv->addr);
            if (!ep) {
                break;
            }

            auto n = incoming.async_push(asio::buffer(recv->data), *ep, yield);
            if (!n) {
                break;
            }
        }
    }

    void send_loop(Async yield) {
        std::vector<uint8_t> buffer;
        asio::ip::udp::endpoint ep;

        while (true) {
            buffer.resize(outgoing.peek());

            auto n = outgoing.async_pop(asio::buffer(buffer), ep, yield);
            if (!n) {
                break;
            }

            buffer.resize(*n);

            auto send = inner.send_to(buffer, util::str(ep), yield);
            if (!send) {
                break;
            }
        }
    }

    void close(Async yield) {
        incoming.close();
        outgoing.close();

        // Flush outgoing messages
        std::vector<uint8_t> buffer;
        asio::ip::udp::endpoint ep;

        while (!outgoing.empty()) {
            buffer.resize(outgoing.peek());
            size_t n = outgoing.try_pop(asio::buffer(buffer), ep);
            buffer.resize(n);

            auto send = inner.send_to(buffer, util::str(ep), yield);
            if (!send) {
                break;
            }
        }

        std::ignore = inner.close(yield);
    }
};

OuisyncSocket::OuisyncSocket(
    std::shared_ptr<State> state,
    Trace trace
)
    : _state(std::move(state))
{
    task::spawn_detached(
        get_executor(),
        [state = _state, trace] (asio::yield_context y) {
            state->send_loop(Async(y, trace));
        }
    );

    task::spawn_detached(
        get_executor(),
        [state = _state, trace] (asio::yield_context y) {
            state->recv_loop(Async(y, trace));
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

    auto ex = inner.get_executor();

    return OuisyncSocket(
        std::make_shared<State>(
            ex,
            std::move(inner),
            *local_endpoint
        ),
        yield.trace()
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
        asio::post(get_executor(), asio::append(std::move(handler), asio::error::shut_down, 0));
        return;
    }

    _state->incoming.async_pop(
        buffers,
        sender,
        std::move(handler)
    );
}

void OuisyncSocket::async_send_to(
    const std::span<const asio::const_buffer>& buffers,
    const endpoint_type& receiver,
    handler handler
) {
    if (!_state) {
        asio::post(get_executor(), asio::append(std::move(handler), asio::error::shut_down, 0));
        return;
    }

    _state->outgoing.async_push(buffers, receiver, std::move(handler));
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

    size_t n = _state->outgoing.try_push(buffers, receiver);

    if (n == 0 && asio::buffer_size(buffers) > 0) {
        ec = asio::error::would_block;
    } else {
        ec = error_code();
    }

    return n;
}

} // namespace ouinet::ouisync_service
