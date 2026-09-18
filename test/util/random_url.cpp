#include "namespaces.h"

#include "http_client.h"
#include "random_url.h"
#include "ssl/util.h"
#include "util/url.h"

using namespace std::string_literals;

namespace ouinet {

std::expected<
    util::Url,
    sys::error_code
>
random_url_from_wikipedia(Async yield)
{
    asio::ssl::context ssl_ctx{asio::ssl::context::tls_client};
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
    auto url = util::Url::from(
        "https://en.wikipedia.org/wiki/Special:Random"s)
    .value();
    auto rs = fetch_from_origin(url, ssl_ctx, yield, http::status::found);
    if (!rs.has_value()) return std::unexpected(rs.error());
    std::string location = rs.value().find(http::field::location)->value();
    return util::Url::from("https:"s + location).value();
}

} // namespace ouinet
