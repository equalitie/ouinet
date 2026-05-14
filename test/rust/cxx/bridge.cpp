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
        rust::Vec<rust::String> argv,
        rust::Str log_tag
    ) {
        std::vector<std::string> argv_s;
        std::transform(argv.begin(), argv.end(), std::back_inserter(argv_s), [](const rust::String& s) {
            return static_cast<std::string>(s);
        });

        std::vector<const char*> argv_p;
        std::transform(argv_s.begin(), argv_s.end(), std::back_inserter(argv_p), [](const std::string& s) {
            return s.c_str();
        });

        return std::make_unique<Client>(
            ctx,
            ClientConfig(argv_p.size(), argv_p.data()),
            util::LogPath(static_cast<std::string>(log_tag))
        );
    }

    SocketAddr get_proxy_endpoint_raw(const Client& client) {
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
