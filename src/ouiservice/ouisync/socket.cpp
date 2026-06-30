#include "socket.h"

#include <boost/algorithm/string/case_conv.hpp>

#include "parse/endpoint.h"

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
    boost::asio::ip::udp::endpoint local_endpoint;

    using queue_sig = void(
        boost::system::error_code,
        std::tuple<endpoint_type, std::vector<uint8_t>>
    );

    boost::asio::experimental::channel<queue_sig> outgoing;
    boost::asio::experimental::channel<queue_sig> incoming;

    size_t available; // total number of bytes in _incoming

    State(
        const asio::any_io_executor& exec,
        ouisync::NetworkSocket inner,
        endpoint_type local_endpoint
    ) :
        inner(std::move(inner)),
        local_endpoint(std::move(local_endpoint)),
        outgoing(exec, outgoing_capacity),
        incoming(exec, incoming_capacity),
        available(0)
    {}

    const asio::any_io_executor& get_executor() {
        return outgoing.get_executor();
    }

    void recv_loop(Async yield) {
        constexpr size_t max_datagram_size = 4096;

        while (true) {
            auto recv = inner.recv_from(max_datagram_size, yield);
            if (!recv) {
                std::ignore = incoming.async_send(
                    recv.error(),
                    {},
                    yield
                );
                break;
            }

            auto ep = parse::endpoint<ip::udp>(recv->addr);
            if (!ep) {
                break;
            }

            auto size = recv->data.size();

            auto send = incoming.async_send(
                error_code(),
                std::make_tuple(*ep, std::move(recv->data)),
                yield
            );
            if (!send) {
                break;
            }

            available += size;
        }
    }

    void send_loop(Async yield) {
        while (true) {
            auto recv = outgoing.async_receive(yield);
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
};

OuisyncSocket::OuisyncSocket(std::shared_ptr<State> state)
    : _state(std::move(state))
{
    task::spawn_detached(
        _state->get_executor(),
        [state = _state] (asio::yield_context y) {
            state->send_loop(Async(y));
        }
    );

    task::spawn_detached(
        _state->get_executor(),
        [state = _state] (asio::yield_context y) {
            state->recv_loop(Async(y));
        }
    );
}

OuisyncSocket::~OuisyncSocket() {
    if (!_state) {
        return;
    }

    _state->incoming.close();
    _state->outgoing.close();

    // Close the inner socket but ignore any errors
    asio::spawn(
        get_executor(),
        [inner = std::move(_state->inner)] (asio::yield_context yield) mutable {
            inner.close(yield);
        },
        asio::detached
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

    return OuisyncSocket(std::make_shared<State>(
        exec,
        std::move(inner),
        *local_endpoint
    ));
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

    return _state->available;
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

    // TODO: cancellation
    _state->incoming.async_receive(
        [this, &buffers, &sender, handler = std::move(handler)]
        (error_code ec, std::tuple<endpoint_type, std::vector<uint8_t>> payload) mutable {
            auto [ payload_ep, payload_data ] = std::move(payload);

            if (ec) {
                handler(ec, 0);
            } else {
                asio::buffer_copy(buffers, asio::buffer(payload_data));
                sender = payload_ep;
                _state->available -= payload_data.size();
                handler(ec, payload_data.size());
            }
        }
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

    // TODO: cancellation
    _state->outgoing.async_send(
        error_code(),
        std::make_tuple(receiver, std::move(data)),
        [size, handler = std::move(handler)] (error_code ec) mutable {
            if (ec) {
                handler(ec, 0);
            } else {
                handler(ec, size);
            }
        }
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

    // TODO: is there a way to check if the send queue is not full before copying the buffers?

    auto size = asio::buffer_size(buffers);
    std::vector<uint8_t> data(size);
    asio::buffer_copy(asio::buffer(data), buffers);

    if (_state->outgoing.try_send(error_code(), std::make_tuple(receiver, std::move(data)))) {
        ec = error_code();
        return size;
    } else {
        ec = asio::error::would_block;
        return 0;
    }
}

} // namespace ouinet::ouisync_service
