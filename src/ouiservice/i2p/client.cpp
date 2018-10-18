#include <I2PTunnel.h>
#include <I2PService.h>

#include "client.h"

#include "../../logger.h"
#include "../../util/condition_variable.h"
#include "../../or_throw.h"

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

Client::Client(std::shared_ptr<Service> service, const string& target_id, uint32_t timeout, asio::io_service& ios)
    : _service(service)
    , _ios(ios)
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
    std::unique_ptr<i2p::client::I2PClientTunnel> i2p_client_tunnel = std::make_unique<i2p::client::I2PClientTunnel>("i2p_oui_client", _target_id, "127.0.0.1", 0, nullptr);
    _client_tunnel = std::make_unique<Tunnel>(_ios, std::move(i2p_client_tunnel), _timeout);

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

ouinet::OuiServiceImplementationClient::ConnectInfo
Client::connect(asio::yield_context yield, Signal<void()>& cancel)
{
    using ConnectInfo = ouinet::OuiServiceImplementationClient::ConnectInfo;

    sys::error_code ec;

    if ((not _client_tunnel) or (not _client_tunnel->is_ready()))
      or_throw(yield, asio::error::operation_aborted);

    Connection connection(_ios);
    
    auto cancel_slot = cancel.connect([&] {
        // tcp::socket::cancel() does not work properly on all platforms
        connection.close();
    });

    LOG_DEBUG("Connecting to the i2p injector...");

    connection._socket.async_connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), _port), yield[ec]);

    if (ec) {
        return or_throw<ConnectInfo>(yield, ec);
    }

    LOG_DEBUG("Connection to the i2p injector is established");

    _client_tunnel->_connections.add(connection);

    return ConnectInfo({ GenericConnection(std::move(connection))
                       , _target_id
                       });
    
}
