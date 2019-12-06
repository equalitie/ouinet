#include "lampshade.h"
#include "../or_throw.h"
#include "../util.h"

#include <fstream>
#include <streambuf>

namespace ouinet {
namespace ouiservice {

LampshadeOuiServiceServer::LampshadeOuiServiceServer(
    const asio::executor& ex,
    asio::ip::tcp::endpoint endpoint,
    boost::filesystem::path state_directory
):
    _ex(ex),
    _endpoint(endpoint)
{
    boost::filesystem::path private_key_file = state_directory/"private.key";
    boost::filesystem::path public_key_file = state_directory/"public.key";
    sys::error_code ec;

    boost::filesystem::create_directories(state_directory, ec);
    if (ec) {
        return;
    }

    if (boost::filesystem::exists(private_key_file) && boost::filesystem::exists(public_key_file)) {
        std::ifstream private_key_stream;
        private_key_stream.open(private_key_file.native(), std::ios::in | std::ios::binary);
        if (private_key_stream.fail()) {
            return;
        }
        std::string private_key = std::string(
            std::istreambuf_iterator<char>(private_key_stream),
            std::istreambuf_iterator<char>()
        );
        if (private_key_stream.fail()) {
            return;
        }

        std::ifstream public_key_stream;
        public_key_stream.open(public_key_file.native(), std::ios::in | std::ios::binary);
        if (public_key_stream.fail()) {
            return;
        }
        std::string public_key = std::string(
            std::istreambuf_iterator<char>(public_key_stream),
            std::istreambuf_iterator<char>()
        );
        if (public_key_stream.fail()) {
            return;
        }

        _private_key_der = std::move(private_key);
        _public_key_der = std::move(public_key);
    } else {
        ouinet::lampshade::generate_key_pair(2048, _private_key_der, _public_key_der);

        std::ofstream private_key_stream;
        private_key_stream.open(private_key_file.native(), std::ios::out | std::ios::binary);
        private_key_stream.write(_private_key_der.data(), _private_key_der.size());
        private_key_stream.close();

        std::ofstream public_key_stream;
        public_key_stream.open(public_key_file.native(), std::ios::out | std::ios::binary);
        public_key_stream.write(_public_key_der.data(), _public_key_der.size());
        public_key_stream.close();
    }
}

LampshadeOuiServiceServer::LampshadeOuiServiceServer(
    const asio::executor& ex,
    asio::ip::tcp::endpoint endpoint,
    std::string private_key_der,
    std::string public_key_der
):
    _ex(ex),
    _endpoint(endpoint),
    _private_key_der(private_key_der),
    _public_key_der(public_key_der)
{}

void LampshadeOuiServiceServer::start_listen(asio::yield_context yield)
{
    sys::error_code ec;

    _listener = std::make_unique<lampshade::Listener>(_ex);
    _listener->listen(_endpoint, _private_key_der, yield[ec]);
    if (ec) {
        _listener.reset();
        return or_throw(yield, ec);
    }
}

void LampshadeOuiServiceServer::stop_listen()
{
    _listener.reset();
}

GenericStream LampshadeOuiServiceServer::accept(asio::yield_context yield)
{
    return _listener->accept(yield);
}

std::string LampshadeOuiServiceServer::public_key() const
{
    return util::base64_encode(_public_key_der);
}

static void parse_endpoint(
    std::string endpoint_string,
    boost::optional<asio::ip::tcp::endpoint>& endpoint,
    std::string& public_key_der
) {
    endpoint = boost::none;
    std::vector<std::string> parts;
    boost::algorithm::split(parts, endpoint_string, [](char c) { return c == ','; });
    if (parts.size() != 2) {
        return;
    }
    if (parts[1].substr(0, 4) != "key=") {
        return;
    }
    public_key_der = util::base64_decode(parts[1].substr(4));


    size_t pos = parts[0].rfind(':');
    if (pos == std::string::npos) {
        return;
    }

    int port;
    try {
        port = std::stoi(parts[0].substr(pos + 1));
    } catch(...) {
        return;
    }
    sys::error_code ec;
    asio::ip::address address = asio::ip::address::from_string(parts[0].substr(0, pos), ec);
    if (ec) {
        return;
    }
    endpoint = asio::ip::tcp::endpoint(address, port);
}

LampshadeOuiServiceClient::LampshadeOuiServiceClient(
    const asio::executor& ex,
    std::string endpoint_string
):
    _ex(ex)
{
    parse_endpoint(endpoint_string, _endpoint, _public_key_der);
}

void LampshadeOuiServiceClient::start(asio::yield_context yield)
{
    if (!_endpoint) {
        return or_throw(yield, asio::error::invalid_argument);
    }

    sys::error_code ec;

    _dialer = std::make_unique<lampshade::Dialer>(_ex);
    _dialer->init(*_endpoint, _public_key_der, yield[ec]);
    if (ec) {
        _dialer.reset();
        return or_throw(yield, ec);
    }
}

void LampshadeOuiServiceClient::stop()
{
    _dialer.reset();
}

GenericStream LampshadeOuiServiceClient::connect(asio::yield_context yield, Signal<void()>& cancel)
{
    if (!_dialer) {
        return or_throw<GenericStream>(yield, asio::error::invalid_argument);
    }

    return _dialer->dial(yield, cancel);
}

} // ouiservice namespace
} // ouinet namespace
