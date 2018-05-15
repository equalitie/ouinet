#include <I2PTunnel.h>

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
    _i2p_tunnel = std::make_unique<i2p::client::I2PClientTunnel>("i2p_oui_client", _target_id, "127.0.0.1", 0, nullptr);

    sys::error_code ec;
    ConditionVariable ready_condition(_ios);

    _i2p_tunnel->AddReadyCallback([&ec, &ready_condition](const sys::error_code& error) mutable {
        ec = error;
        ready_condition.notify();
    });

    _i2p_tunnel->Start();
    _port = _i2p_tunnel->GetLocalEndpoint().port();
    _i2p_tunnel->SetConnectTimeout(_timeout);

    ready_condition.wait(yield);
    if (ec) {
        or_throw(yield, ec);
    }

    LOG_DEBUG("I2P Tunnel has been established");
}

void Client::stop()
{
    if (_i2p_tunnel) {
        _i2p_tunnel->Stop();
        _i2p_tunnel = nullptr;
    }

    _connections.close_all();
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

    _connections.add(connection);

    return ConnectInfo({ GenericConnection(std::move(connection))
                       , _target_id
                       });
}
