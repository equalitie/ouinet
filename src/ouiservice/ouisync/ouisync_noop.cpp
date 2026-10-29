#include "ouisync.h"

namespace ouinet::ouisync_service {

Ouisync::Ouisync(
    const util::AsioExecutor& exec,
    boost::filesystem::path,
    std::string page_index_token,
    std::vector<boost::asio::ip::udp::endpoint>
) :
    _impl_cv(exec)
{}

[[nodiscard]]
sys::error_code Ouisync::start(Async) {
    return asio::error::operation_not_supported;
}

void Ouisync::stop() {}

bool Ouisync::is_running() const { return false; }

[[nodiscard]]
std::expected<Session, sys::error_code> Ouisync::load(const CacheOuisyncRetrieveRequest&, Async) {
    return std::unexpected(asio::error::operation_not_supported);
}

[[nodiscard]]
std::expected<std::vector<OuisyncSocket>, sys::error_code> Ouisync::open_network_sockets(Async) {
    return std::unexpected(asio::error::operation_not_supported);
}

void init_log() {}
bool is_implemented() { return false; }

} // namespace
