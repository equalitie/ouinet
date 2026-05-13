#include <I2PService.h>
#include "server.h"
#include "service.h"

#include <Destination.h>
#include <I2PTunnel.h>
#include <Identity.h>
#include <api.h>

#include <fstream>
#include <streambuf>

#include "or_throw.h"
#include "handshake.h"
#include "async_sleep.h"


namespace ouinet::i2p_direct {

using namespace std;

Server::Server(std::shared_ptr<Service> service, const string& private_key_filename, uint32_t timeout, const executor_type& exec)
    : _service(service)
    , _exec(exec)
    , _timeout(timeout)
    , _tcp_acceptor(exec)
{
    load_private_key(private_key_filename);
}

void Server::load_private_key(const string& key_file_name)
{
    ifstream in_file(key_file_name);
    string keys_str;
    OUI_LOG_DEBUG("Reading private key from" + key_file_name);
    if (in_file.is_open()) {
        keys_str = string( istreambuf_iterator<char>(in_file)
                         , istreambuf_iterator<char>());
        
        
    } else {
        // File doesn't exist
        i2p::data::SigningKeyType sig_type = i2p::data::SIGNING_KEY_TYPE_ECDSA_SHA256_P256;
        i2p::data::PrivateKeys keys = i2p::data::PrivateKeys::CreateRandomKeys(sig_type);
        keys_str = keys.ToBase64();

        ofstream out_file(key_file_name);
        out_file << keys_str;
    }

    _private_keys = std::make_unique<i2p::data::PrivateKeys>();
    _private_keys->FromBase64(keys_str);
}

Server::~Server()
{
    stop_listen();
}

sys::error_code Server::start_listen(Async yield)
{
    auto slot = _stopped.connect([&] { yield.cancel(); });

    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 0);

    sys::error_code ec;

    /// announce that we started listening on i2p port
    OUI_LOG_DEBUG("I2P server openning port..");

    _tcp_acceptor.open(endpoint.protocol(), ec);
    if (ec) return ec;

    _tcp_acceptor.set_option(asio::socket_base::reuse_address(true));

    _tcp_acceptor.bind(endpoint, ec);

    if (ec) {
        _tcp_acceptor.close();
        return ec;
    }

    _tcp_acceptor.listen(asio::socket_base::max_listen_connections, ec);

    if (ec) {
        _tcp_acceptor.close();
        return ec;
    }

    uint16_t port = _tcp_acceptor.local_endpoint().port();

    auto& tunnel_params = _service->get_tunnel_params();
    _local_destination = i2p::api::CreateLocalDestination(*_private_keys, true,
        tunnel_params.IsEmpty() ? nullptr : &tunnel_params);

    do {
        auto i2p_tunnel = std::make_unique<i2p::client::I2PServerTunnel>("i2p_oui_server", "127.0.0.1", port, _local_destination);
        _server_tunnel = std::make_unique<Tunnel>(_exec, std::move(i2p_tunnel), _timeout);
        sys::error_code ec = _server_tunnel->wait_to_get_ready(yield);
        if (ec) {
            OUI_LOG_DEBUG("I2P server tunnel setup attempt failed; ec=", ec.message());
            async_sleep(200ms, yield);
        }
    }
    while(ec);

    return ec;
}

void Server::stop_listen()
{
    _stopped();

    _server_tunnel.reset();

    if (_tcp_acceptor.is_open()) {
        _tcp_acceptor.close();
    }
}

std::expected<GenericStream, sys::error_code>
Server::accept(Async yield) {
    auto slot = _stopped.connect([&] { yield.cancel(); });

    auto conn = accept_without_handshake(yield);

    if (!conn.has_value()) {
        return std::unexpected(conn.error());
    }

    OUI_LOG_DEBUG("I2P server: accepted connection from ", conn->remote_endpoint());

    sys::error_code ec = perform_handshake(*conn, yield);

    if (ec) {
        return std::unexpected(ec);
    }

    return std::move(*conn);
}

std::expected<GenericStream, sys::error_code>
Server::accept_without_handshake(Async yield)
{
    auto slot = _stopped.connect([&] { yield.cancel(); });

    Connection connection(_exec);

    sys::error_code ec = _tcp_acceptor.async_accept(connection.socket(), yield);

    if (!ec && !_server_tunnel) {
        ec = asio::error::operation_aborted;
    }

    if (ec) {
        return std::unexpected(ec);
    }

    _server_tunnel->intrusive_add(connection);
    return GenericStream(std::move(connection));
}

I2pAddress Server::public_identity() const
{
    return *I2pAddress::parse(_private_keys->GetPublic()->ToBase64());
}

} // namespaces
