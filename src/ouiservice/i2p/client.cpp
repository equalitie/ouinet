#include <chrono>

#include <I2PTunnel.h>
#include <I2PService.h>

#include "client.h"
#include "service.h"
#include "handshake.h"

#include "../../logger.h"
#include "../../util/condition_variable.h"
#include "../../or_throw.h"
#include "../../async_sleep.h"

#include "../../namespaces.h"

namespace ouinet::ouiservice::i2poui {

using namespace std;

Client::Client(std::shared_ptr<Service> service, const string& target_id, uint32_t timeout, const AsioExecutor& exec)
    : _service(service)
    , _exec(exec)
    , _target_id(target_id)
    , _timeout(timeout)
{}

Client::~Client()
{
    stop();
}

void Client::start(asio::yield_context yield)
{
    Cancel stopped = _stopped;

    sys::error_code ec;

    do {
        auto i2p_client_tunnel = std::make_unique<i2p::client::I2PClientTunnel>(
                "i2p_oui_client",
                _target_id,
                "127.0.0.1",
                0,
                _service ? _service->get_local_destination () : nullptr);

        _client_tunnel = std::make_unique<Tunnel>(_exec, std::move(i2p_client_tunnel), _timeout);

        _client_tunnel->wait_to_get_ready(yield[ec]);
    } while(_client_tunnel->has_timed_out() && !stopped);

    if (!ec && !_client_tunnel) ec = asio::error::operation_aborted;
    ec = compute_error_code(ec, stopped);
    if (ec) return or_throw(yield, ec);

    _port = _client_tunnel->local_endpoint().port();
}

void Client::stop()
{
    _client_tunnel.reset();
    //tunnel destructor will stop the i2p tunnel after the connections
    //are closed. (TODO: maybe we need to add a wait here)
    _stopped();
}

inline void exponential_backoff(AsioExecutor& exec, uint32_t i, Cancel& cancel, asio::yield_context yield) {
    // Constants in this function are made up, feel free to modify them as needed.
    if (i < 3) return;
    i -= 3;
    uint32_t constant_after = 8; // max 12.8 seconds
    if (i > constant_after) i = constant_after;
    float delay_s = powf(2, i) / 10.f;

    if (!async_sleep(exec, chrono::milliseconds(long(delay_s * 1000.f)), cancel, yield)) {
        return or_throw(yield, asio::error::operation_aborted);
    }
}

::ouinet::GenericStream
Client::connect(asio::yield_context yield, Cancel& cancel)
{
    for (uint32_t i = 0;; ++i) {
        sys::error_code ec;
        auto conn = connect_without_handshake(yield[ec], cancel);

        if (!ec) {
            auto stopped = _stopped.connect([&cancel] { cancel(); });
            perform_handshake(conn, cancel, yield[ec]);

            if (!ec) {
                return conn;
            }
        }

        if (ec == asio::error::operation_aborted) {
            return or_throw<GenericStream>(yield, ec);
        }

        assert(ec);

        ec = {};
        exponential_backoff(_exec, i, cancel, yield[ec]);

        if (ec) {
            return or_throw<GenericStream>(yield, ec);
        }
    }
}

::ouinet::GenericStream
Client::connect_without_handshake(asio::yield_context yield, Cancel& cancel)
{
    auto stopped = _stopped.connect([&cancel] { cancel(); });

    Connection connection(_exec);
    
    auto cancel_slot = cancel.connect([&] {
        // tcp::socket::cancel() does not work properly on all platforms
        connection.close();
    });

    LOG_DEBUG("Connecting to the i2p injector...");

    for (uint32_t i = 0;; ++i) {
        sys::error_code ec;

        connection._socket.async_connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), _port), yield[ec]);
        ec = compute_error_code(ec, cancel);

        if (ec == asio::error::operation_aborted) {
            return or_throw<GenericStream>(yield, ec);
        }

        if (ec) {
            ec = {};
            exponential_backoff(_exec, i, cancel, yield[ec]);
            if (ec) return or_throw<GenericStream>(yield, ec);
            continue;
        }

        LOG_DEBUG("Connection to the i2p injector is established");

        _client_tunnel->intrusive_add(connection);

        return GenericStream{move(connection)};
    }
}

} // namespaces
