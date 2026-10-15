#include "http_client.h"
#include "util/url.h"
#include "util/async.h"
#include "ssl/util.h"
#include "request_builder.h"
#include <boost/system.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/http/read.hpp>

namespace ouinet {

asio::ssl::stream<boost::beast::tcp_stream> setup_tls_stream(asio::ip::tcp::socket socket, asio::ssl::context& ctx, std::string host) {
    asio::ssl::stream<boost::beast::tcp_stream> stream(std::move(socket), ctx);
    if(! SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
        sys::error_code ec;
        ec.assign(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category());
        static boost::source_location loc = BOOST_CURRENT_LOCATION;
        sys::throw_exception_from_error(ec, loc);
    }
    stream.set_verify_callback(asio::ssl::host_name_verification(host));
    return stream;
}

std::expected<
    http::response<http::string_body>,
    sys::error_code
>
fetch_from_origin(util::Url url, asio::ssl::context& ctx, Async yield) {
    if (url.port.empty()) url.port = "443";
    if (url.path.empty()) url.path = "/";

    auto exec = yield.get_executor();

    asio::ip::tcp::resolver resolver(exec);
    auto const results = resolver.async_resolve(url.host, url.port, yield);
    if (!results) return std::unexpected(results.error());

    auto req = build_origin_request(url);
    std::string host = req[http::field::host];

    asio::ip::tcp::socket socket(exec);
    if (auto r = asio::async_connect(socket, *results, yield); !r) {
        return std::unexpected(r.error());
    }

    auto stream = setup_tls_stream(std::move(socket), ctx, url.host);
    if (auto r = stream.async_handshake(asio::ssl::stream_base::client, yield); !r) {
        return std::unexpected(r.error());
    }

    if (auto r = http::async_write(stream, req, yield); !r) {
        return std::unexpected(r.error());
    }

    beast::flat_buffer b;
    http::response<http::string_body> res;
    if (auto r = http::async_read(stream, b, res, yield); !r) {
        return std::unexpected(r.error());
    }

    std::ignore = stream.async_shutdown(yield);

    assert(res.result() == http::status::ok);

    return res;
}

std::expected<
    http::response<http::string_body>,
    sys::error_code
>
fetch_from_origin(util::Url url, Async yield) {
    asio::ssl::context ctx{asio::ssl::context::tls_client};
    ouinet::ssl::util::load_tls_ca_certificates(ctx);
    ctx.set_verify_mode(asio::ssl::verify_peer);

    return fetch_from_origin(std::move(url), ctx, yield);
}

} // namespace
