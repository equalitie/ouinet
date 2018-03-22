#include "server.h"

#include <Destination.h>
#include <I2PTunnel.h>
#include <Identity.h>
#include <api.h>

#include <fstream>
#include <streambuf>

#include "../../or_throw.h"

using namespace std;
using namespace ouinet::ouiservice;
using namespace ouinet::ouiservice::i2poui;

Server::Server(std::shared_ptr<Service> service, const string& private_key_filename, uint32_t timeout, asio::io_service& ios)
    : _service(service)
    , _ios(ios)
    , _timeout(timeout)
    , _tcp_acceptor(ios)
{
    load_private_key(private_key_filename);
}

void Server::load_private_key(const string& key_file_name)
{
    ifstream in_file(key_file_name);
    string keys_str;
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

void Server::start_listen(asio::yield_context yield)
{
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 0);

    sys::error_code ec;

    _tcp_acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        return or_throw(yield, ec);
    }

    _tcp_acceptor.set_option(asio::socket_base::reuse_address(true));

    _tcp_acceptor.bind(endpoint, ec);
    if (ec) {
        _tcp_acceptor.close();
        return or_throw(yield, ec);
    }

    _tcp_acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) {
        _tcp_acceptor.close();
        return or_throw(yield, ec);
    }

    uint16_t port = _tcp_acceptor.local_endpoint().port();

    std::shared_ptr<i2p::client::ClientDestination> local_dst = i2p::api::CreateLocalDestination(*_private_keys, true);
    _i2p_tunnel = std::make_unique<i2p::client::I2PServerTunnel>("i2p_oui_server", "127.0.0.1", port, local_dst);
    _i2p_tunnel->Start();
    _i2p_tunnel->SetConnectTimeout(_timeout);
}

void Server::stop_listen()
{
    if (_i2p_tunnel) {
        _i2p_tunnel->Stop();
        _i2p_tunnel = nullptr;
    }
    _tcp_acceptor.close();
    _connections.close_all();
}

ouinet::GenericConnection Server::accept(asio::yield_context yield)
{
    sys::error_code ec;

    Connection connection(_ios);
    _tcp_acceptor.async_accept(connection.socket(), yield);
    if (ec) {
        return or_throw<GenericConnection>(yield, ec, GenericConnection());
    }

    _connections.add(connection);
    return GenericConnection(std::move(connection));
}

std::string Server::public_identity() const
{
    return _private_keys->GetPublic()->ToBase64();
}
