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
  _client_tunnel = std::make_unique<Tunnel>(_ios, std::make_unique<i2p::client::I2PClientTunnel>("i2p_oui_client", _target_id, "127.0.0.1", 0, nullptr), _timeout);

  sys::error_code ec;

  _client_tunnel->wait_to_get_ready(yield);
  if (ec) {
    or_throw(yield, ec);
  }

  //The client_tunnel can't return its port becaues it doesn't know
  //that it is a client i2p tunnel, all it knows is that it is an
  //i2ptunnel holding some connections but doesn't know how connections
  //are created.
  _port = dynamic_cast<i2p::client::I2PClientTunnel*>(_client_tunnel->_i2p_tunnel.get())->GetLocalEndpoint().port();

  LOG_DEBUG("I2P Tunnel has been established");
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

    Connection connection(_ios);

    auto cancel_slot = cancel.connect([&] {
        // tcp::socket::cancel() does not work properly on all platforms
        connection.close();
    });

    connection.socket().async_connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), _port), yield[ec]);

    if (ec) {
        return or_throw<ConnectInfo>(yield, ec);
    }

    _client_tunnel->_connections.add(connection);

    return ConnectInfo({ GenericConnection(std::move(connection))
                       , _target_id
                       });
    
}
