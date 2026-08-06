#include "i2pd.h"
#include "logger.h"
#include "util/log_path.h"

#include "Daemon.h"
#include <boost/asio/error.hpp>
#include <thread>

#ifndef OUINET_WITH_I2PD_LIB
#error "Compiling i2pd_lib.cpp but OUINET_WITH_I2PD_LIB is not defined"
#endif

namespace ouinet {

bool I2pd::is_start_lib_implemented() { return true; }

struct I2pd::InnerLib : I2pd::InnerBase {
    std::thread thread;
    util::LogPath log_path;

    InnerLib(std::thread thread, util::LogPath log_path):
        thread(std::move(thread)),
        log_path(std::move(log_path))
    {}

    ~InnerLib() {
        LOG_DEBUG(log_path, " Stopping I2P daemon");
        // In the i2pd's UnixDaemon this is set to `false` when SIGINT is received.
        Daemon.running = false;
        Daemon.stop();
        LOG_DEBUG(log_path, " Joining I2P daemon thread");
        thread.join();
    }
};

std::expected<I2pd, sys::error_code>
I2pd::start_lib(I2pd::Config config, util::LogPath log_path) {
    LOG_DEBUG(log_path, " Starting I2P daemon (library)");

    auto config_vec = config.to_vector();

    std::vector<const char*> args;

    args.push_back("i2pd");

    std::transform(
            config_vec.begin(),
            config_vec.end(),
            std::back_inserter(args),
            [] (const std::string& str) { return str.c_str(); });

    if (!Daemon.init(args.size(), (char**) args.data())) {
        LOG_WARN(log_path, " Failed to initialize I2P daemon");
        return std::unexpected(asio::error::fault);
    }

    if (!Daemon.start()) {
        LOG_WARN(log_path, " Failed to start I2P daemon");
        Daemon.stop();
        return std::unexpected(asio::error::fault);
    }

    return I2pd(std::make_unique<InnerLib>(
        std::thread([] { Daemon.run(); }),
        log_path
    ));
}

} // namespace
