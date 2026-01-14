#include <boost/beast/core.hpp>

#include "declspec.h"
#include "cache/http_sign.h"
#include "namespaces.h"
#include "util.h"
#include "http_util.h"
#include "http_logger.h"
#include "util/yield.h"
#include "util/log_path.h"
#include "injector_config.h"
#include "bittorrent/mock_dht.h"

namespace ouinet {

using TcpLookup = asio::ip::tcp::resolver::results_type;

OUINET_DECL TcpLookup
resolve_target(const http::request_header<>& req
              , bool allow_private_targets
              , bool do_doh
              , AsioExecutor exec
              , Cancel& cancel
              , YieldContext yield);

// This class needs to outlive the `asio::io_context`. Mainly because of the
// `ssl::context` which is passed to `ssl::stream`s by reference.
class OUINET_DECL Injector {
public:
    Injector(
        InjectorConfig config,
        asio::io_context& ctx,
        // For use in tests
        util::LogPath log_path = {},
        std::shared_ptr<bittorrent::MockDht> dht = nullptr);

    void stop();
    ~Injector();

    const InjectorConfig& config() const {
        return _config;
    }

    std::string cache_http_public_key() const {
        std::ifstream file((_config.repo_root() / "ed25519-public-key").string());
        if (!file) throw std::runtime_error("File not found");
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    fs::path tls_cert_file() const {
        return config().repo_root() / "tls-cert.pem";
    }

private:
    InjectorConfig _config;
    Cancel _cancel;
    std::shared_ptr<bittorrent::DhtBase> _dht;
    std::unique_ptr<asio::ssl::context> _ssl_context;
};

} // namespace ouinet
