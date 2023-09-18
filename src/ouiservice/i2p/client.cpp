#include <I2PTunnel.h>
#include <I2PService.h>

#include "client.h"
#include "service.h"

#include "../../logger.h"
#include "../../util/condition_variable.h"
#include "../../or_throw.h"

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

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
  sys::error_code ec;

  do {
    std::unique_ptr<i2p::client::I2PClientTunnel> i2p_client_tunnel = std::make_unique<i2p::client::I2PClientTunnel>("i2p_oui_client", _target_id, "127.0.0.1", 0, _service ? _service->get_local_destination () : nullptr);
    _client_tunnel = std::make_unique<Tunnel>(_exec, std::move(i2p_client_tunnel), _timeout);

    _client_tunnel->wait_to_get_ready(yield[ec]);
  } while(_client_tunnel->has_timed_out());

    if (!ec && !_client_tunnel) ec = asio::error::operation_aborted;
    if (ec) return or_throw(yield, ec);

  //The client_tunnel can't return its port becaues it doesn't know
  //that it is a client i2p tunnel, all it knows is that it is an
  //i2ptunnel holding some connections but doesn't know how connections
  //are created.
  _port = dynamic_cast<i2p::client::I2PClientTunnel*>(_client_tunnel->_i2p_tunnel.get())->GetLocalEndpoint().port();
}

void Client::stop()
{
  _client_tunnel.reset();
  //tunnel destructor will stop the i2p tunnel after the connections
  //are closed. (TODO: maybe we need to add a wait here)

}

::ouinet::GenericStream
Client::connect(asio::yield_context yield, Signal<void()>& cancel)
{
    sys::error_code ec;

    Connection connection(_exec);
    
    auto cancel_slot = cancel.connect([&] {
        // tcp::socket::cancel() does not work properly on all platforms
        connection.close();
    });

    LOG_DEBUG("Connecting to the i2p injector...");

    connection._socket.async_connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), _port), yield[ec]);

    if (ec) {
        return or_throw<GenericStream>(yield, ec);
    }

    LOG_DEBUG("Connection to the i2p injector is established");

    _client_tunnel->_connections.add(connection);

    return GenericStream{move(connection)};
}
