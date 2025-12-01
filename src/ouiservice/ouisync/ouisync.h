#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/filesystem/path.hpp>
#include "namespaces.h"
#include "or_throw.h"
#include "util/yield.h"

namespace ouinet {
    class GenericStream; 
}

namespace ouinet::ouisync_service {

#ifdef WITH_OUISYNC

class Ouisync {
public:
    Ouisync(boost::filesystem::path);
    Ouisync(const Ouisync&) = delete;
    Ouisync(Ouisync&&) = default;
    Ouisync operator=(const Ouisync&) = delete;

    void start(boost::asio::yield_context);
    void stop();

    bool is_running() const;

    void serve(
        GenericStream&,
        const http::request_header<>&,
        YieldContext
    );

private:
    boost::filesystem::path _service_dir;
    boost::filesystem::path _store_dir;
    boost::filesystem::path _mount_dir;
    struct Impl;
    std::shared_ptr<Impl> _impl;
};

#else // ifdef WITH_OUISYNC
 
class Ouisync {
public:
    Ouisync(boost::filesystem::path) {}
    Ouisync(const Ouisync&) = delete;
    Ouisync(Ouisync&&) = default;
    Ouisync operator=(const Ouisync&) = delete;

    void start(boost::asio::yield_context yield) {
        return or_throw(yield, asio::error::operation_not_supported);
    }

    void stop() {}

    bool is_running() const { return false; }

    void serve(
        GenericStream&,
        const http::request_header<>&,
        YieldContext
    ) {
        return or_throw(yield, asio::error::operation_not_supported);
    }
};

#endif // ifdef WITH_OUISYNC

} // namespace ouinet::ouisync_service
