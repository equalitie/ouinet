#include "connect_proxy.h"
#include "../or_throw.h"
#include <boost/beast/http.hpp>
#include <boost/beast/core.hpp>
#include <iostream>

namespace ouinet {
namespace ouiservice {

using namespace std;


std::expected<GenericStream, sys::error_code>
ConnectProxyOuiServiceClient::connect(Async yield)
{
    auto connection = _base->connect(yield);

    if (!connection.has_value()) {
        return std::unexpected(connection.error());
    }

    auto cancel_slot = yield.cancel_slot([&] { connection->close(); });

    http::request<http::empty_body> req{http::verb::connect, "injector", 11};

    auto w_result = http::async_write(*connection, req, yield);

    if (!w_result.has_value()) {
        return std::unexpected(w_result.error());
    }

    beast::flat_buffer b;
    http::response<http::empty_body> res;

    auto r_result = http::async_read(*connection, b, res, yield);

    if (r_result.has_value() && res.result() != http::status::ok) {
        return std::unexpected(asio::error::connection_reset);
    }

    if (!r_result.has_value()) {
        return std::unexpected(r_result.error());
    }

    return std::move(*connection);
}


} // ouiservice namespace
} // ouinet namespace
