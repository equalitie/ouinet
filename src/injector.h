#include <boost/beast/core.hpp>

#include "cache/http_sign.h"
#include "namespaces.h"
#include "util.h"
#include "http_util.h"
#include "http_logger.h"
#include "util/yield.h"
#include "injector_config.h"
#include "bittorrent/dht.h"


// TODO: Don't do this in global namespaces nor headers
using namespace std;

namespace ouinet {

using TcpLookup = asio::ip::tcp::resolver::results_type;

TcpLookup
resolve_target(const http::request_header<>& req
              , bool allow_private_targets
              , AsioExecutor exec
              , Cancel& cancel
              , Yield yield);

// This class needs to outlive the `asio::io_context`. Mainly because of the
// `ssl::context` which is passed to `ssl::stream`s by reference.
class Injector {
public:
    Injector(InjectorConfig config, asio::io_context& ctx);
    void stop();
    ~Injector();

private:
    Cancel _cancel;
    std::shared_ptr<bittorrent::MainlineDht> _dht;
    std::unique_ptr<asio::ssl::context> _ssl_context;
};

} // namespace ouinet
