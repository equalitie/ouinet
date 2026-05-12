#include "bridge.hpp"

namespace ouinet {
namespace test {
    std::unique_ptr<IoContext> new_io_context() {
        return std::make_unique<IoContext>();
    }

    std::unique_ptr<Client> new_client(IoContext& ctx, rust::Slice<const rust::Str> options) {
        // TODO

        return std::make_unique<Client>(
            ctx,
            ClientConfig{}
        );
    }

} // namespace test
} // namespace ouinet
