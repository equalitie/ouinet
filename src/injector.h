#pragma once

#include <boost/beast/core.hpp>
#include <boost/asio/ssl/context.hpp>

#include "cache/http_sign.h"
#include "namespaces.h"
#include "util.h"
#include "http_util.h"
#include "http_logger.h"
#include "util/executor.h"
#include "util/yield.h"
#include "util/log_path.h"
#include "util/promise.h"
#include "injector_config.h"
#include "bittorrent/mock_dht.h"
#include "ouiservice/i2p/address.h"
#include "api.h"

namespace ouinet {

using TcpLookup = asio::ip::tcp::resolver::results_type;

OUINET_INJECTOR_API
TcpLookup
resolve_target(const http::request_header<>& req
              , bool allow_private_targets
              , std::shared_ptr<dns::Resolver> dns_resolver
              , AsioExecutor exec
              , Cancel& cancel
              , YieldContext yield);

// This class needs to outlive the `asio::io_context`. Mainly because of the
// `ssl::context` which is passed to `ssl::stream`s by reference.
class OUINET_INJECTOR_API Injector {
public:
    Injector(
        InjectorConfig config,
        asio::io_context& ctx,
        // For use in tests
        util::LogPath log_path = {},
        std::shared_ptr<bittorrent::MockDht> dht = nullptr);

    void stop();
    ~Injector();

    AsioExecutor get_executor() const noexcept {
        return _exec;
    }

    const InjectorConfig& config() const {
        return _config;
    }

    std::string cache_http_public_key() const {
#ifdef __APPLE__
        std::ifstream file((_config.repo_root() / "ed25519-public-key").string());
#else
        std::ifstream file((_config.repo_root() / "ed25519-public-key"));
#endif
        if (!file) throw std::runtime_error("File not found");
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    fs::path tls_cert_file() const {
        return config().repo_root() / "tls-cert.pem";
    }

    std::optional<I2pAddress> i2p_address(Async);

private:
    struct Inner;

    AsioExecutor _exec;
    InjectorConfig _config;
    std::shared_ptr<dns::Resolver> _dns_resolver;
    Cancel _cancel;
    std::shared_ptr<bittorrent::DhtBase> _dht;
    std::unique_ptr<asio::ssl::context> _ssl_context;
    std::optional<I2pAddress> _i2p_address;

    // TODO: Move all of the above inside `_inner` to use the pimpl pattern.
    std::unique_ptr<Inner> _inner;
};

} // namespace ouinet
