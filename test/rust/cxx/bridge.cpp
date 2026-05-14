#include "bridge.hpp"
#include "ouinet-test-rs/src/lib.rs.h"
#include "util/log_path.h"

namespace util = ouinet::util;

namespace ouinet {
namespace test {
    std::unique_ptr<Context> new_context() {
        return std::make_unique<Context>();
    }

    std::unique_ptr<Client> new_client(
        Context& ctx,
        rust::Slice<const char* const> argv,
        rust::Str log_tag
    ) {
        return std::make_unique<Client>(
            ctx,
            ClientConfig(argv.size(), const_cast<const char**>(argv.data())),
            util::LogPath(static_cast<std::string>(log_tag))
        );
    }

    void stop(Client& client, rust::Box<Completer> completer) {
        boost::asio::post(client.get_executor(), [client, completer = std::move(completer)]() mutable {
            if (completer->is_closed()) {
                return;
            }

            client.stop();
            completer->complete();
        });
    }

    SocketAddr get_proxy_endpoint(const Client& client) {
        auto ep = client.get_proxy_endpoint();

        if (ep.address().is_v4()) {
            auto bytes = ep.address().to_v4().to_bytes();

            return SocketAddr {
                IpFamily::V4,
                std::array<unsigned char, 16> {
                    bytes[0], bytes[1], bytes[2], bytes[3],
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
                },
                ep.port()
            };
        } else {
            return SocketAddr {
                IpFamily::V6,
                ep.address().to_v6().to_bytes(),
                ep.port()
            };
        }
    }

} // namespace test
} // namespace ouinet
