#pragma once

#include "request.h"
#include "session.h"
#include "api.h"
#include <boost/asio/ip/address_v4.hpp>
#include <expected>

namespace ouinet { class Async; }

#ifdef WITH_OUISYNC

namespace ouisync { class FileStream; }

namespace ouinet {
namespace ouisync_service {

class OUINET_OUISYNC_API Ouisync {
public:
    Ouisync(
        boost::filesystem::path,
        std::string page_index_token,
        std::vector<boost::asio::ip::udp::endpoint> bind = {
            boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::any(), 0)
        }
    );
    Ouisync(const Ouisync&) = delete;
    Ouisync(Ouisync&&) = default;
    Ouisync operator=(const Ouisync&) = delete;

    [[nodiscard]]
    sys::error_code start(Async);
    void stop();

    bool is_running() const;

    [[nodiscard]]
    std::expected<Session, sys::error_code> load(const CacheOuisyncRetrieveRequest&, Async);

private:
    boost::filesystem::path _service_dir;
    boost::filesystem::path _store_dir;
    boost::filesystem::path _mount_dir;
    std::vector<boost::asio::ip::udp::endpoint> _bind;
    struct Impl;
    std::shared_ptr<Impl> _impl;
    std::string _page_index_token;
};

} // namespace ouisync_service

namespace util::file_io {
    [[nodiscard]] std::expected<size_t, sys::error_code> file_size(ouisync::FileStream& file);
    [[nodiscard]] std::expected<void, sys::error_code> fseek(ouisync::FileStream& file, size_t pos);
} // namespace util::file_io

} // namespace ouinet

#else // ifdef WITH_OUISYNC

namespace ouinet::ouisync_service {

class OUINET_OUISYNC_API Ouisync {
public:
    Ouisync(boost::filesystem::path, std::string page_index_token) {}
    Ouisync(const Ouisync&) = delete;
    Ouisync(Ouisync&&) = default;
    Ouisync operator=(const Ouisync&) = delete;

    [[nodiscard]]
    sys::error_code start(Async) {
        return asio::error::operation_not_supported;
    }

    void stop() {}

    bool is_running() const { return false; }

    [[nodiscard]]
    std::expected<Session, sys::error_code> load(const CacheOuisyncRetrieveRequest&, Async) {
        return std::unexpected(asio::error::operation_not_supported);
    }
};

} // namespace ouinet::ouisync_service

#endif // ifdef WITH_OUISYNC
