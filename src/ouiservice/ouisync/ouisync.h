#pragma once

#include "request.h"
#include "session.h"

#ifdef WITH_OUISYNC

#include <ouisync/file_stream.hpp>

namespace ouinet {
namespace ouisync_service {

class Ouisync {
public:
    Ouisync(boost::filesystem::path, std::string page_index_token);
    Ouisync(const Ouisync&) = delete;
    Ouisync(Ouisync&&) = default;
    Ouisync operator=(const Ouisync&) = delete;

    void start(boost::asio::yield_context);
    void stop();

    bool is_running() const;

    Session load(
        const CacheOuisyncRetrieveRequest&,
        YieldContext
    );

private:
    boost::filesystem::path _service_dir;
    boost::filesystem::path _store_dir;
    boost::filesystem::path _mount_dir;
    struct Impl;
    std::shared_ptr<Impl> _impl;
    std::string _page_index_token;
};

} // namespace ouisync_service

namespace util::file_io {

inline size_t file_size(ouisync::FileStream& file, sys::error_code& ec) {
    return file.size();
}

inline void fseek(ouisync::FileStream& file, size_t pos, sys::error_code& ec) {
    file.seek(pos);
}

} // namespace util::file_io
} // namespace ouinet

#else // ifdef WITH_OUISYNC

namespace ouinet::ouisync_service {

class Ouisync {
public:
    Ouisync(boost::filesystem::path, std::string page_index_token) {}
    Ouisync(const Ouisync&) = delete;
    Ouisync(Ouisync&&) = default;
    Ouisync operator=(const Ouisync&) = delete;

    void start(boost::asio::yield_context yield) {
        return or_throw(yield, asio::error::operation_not_supported);
    }

    void stop() {}

    bool is_running() const { return false; }

    Session load(
        const CacheOuisyncRetrieveRequest&,
        YieldContext yield
    ) {
        return or_throw<Session>(yield, asio::error::operation_not_supported);
    }
};

} // namespace ouinet::ouisync_service

#endif // ifdef WITH_OUISYNC
