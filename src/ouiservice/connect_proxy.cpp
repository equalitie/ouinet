#include "connect_proxy.h"
#include "../or_throw.h"
#include <boost/beast/http.hpp>
#include <boost/beast/core.hpp>
#include <iostream>

namespace ouinet {
namespace ouiservice {

using namespace std;


GenericStream
ConnectProxyOuiServiceClient::connect(asio::yield_context yield, Signal<void()>& cancel)
{
    sys::error_code ec;

    auto connection = _base->connect(yield[ec], cancel);

    if (ec) {
        return or_throw<GenericStream>(yield, ec);
    }

    auto cancelled = cancel.connect([&] { connection.close(); });

    http::request<http::empty_body> req{http::verb::connect, "injector", 11};

    http::async_write(connection, req, yield[ec]);

    if (ec) {
        return or_throw<GenericStream>(yield, ec);
    }

    beast::flat_buffer b;
    http::response<http::empty_body> res;

    http::async_read(connection, b, res, yield[ec]);

    if (!ec && res.result() != http::status::ok) {
        ec = asio::error::connection_reset;
    }

    if (ec) {
        return or_throw<GenericStream>(yield, ec);
    }

    return connection;
}


} // ouiservice namespace
} // ouinet namespace
