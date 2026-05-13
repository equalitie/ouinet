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

} // namespace test
} // namespace ouinet
