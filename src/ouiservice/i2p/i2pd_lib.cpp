#include "i2pd.h"
#include "logger.h"
#include "util/log_path.h"

#include "Daemon.h"
#include <boost/asio/error.hpp>

#ifndef OUINET_WITH_I2PD_LIB
#error "Compiling i2pd_lib.cpp but OUINET_WITH_I2PD_LIB is not defined"
#endif

namespace ouinet {

bool I2pd::is_start_lib_implemented() { return true; }

struct OuiDaemon : public i2p::util::Daemon_Singleton {
    static OuiDaemon& instance() {
        static OuiDaemon d;
        return d;
    }
};

struct I2pd::InnerLib : I2pd::InnerBase {
    util::LogPath log_path;

    InnerLib(util::LogPath log_path):
        log_path(std::move(log_path))
    {}

    ~InnerLib() {
        LOG_DEBUG(log_path, " Stopping I2P daemon");
        OuiDaemon::instance().stop();
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

    if (!OuiDaemon::instance().init(args.size(), (char**) args.data())) {
        LOG_WARN(log_path, " Failed to initialize I2P daemon");
        return std::unexpected(asio::error::fault);
    }

    if (!OuiDaemon::instance().start()) {
        LOG_WARN(log_path, " Failed to start I2P daemon");
        OuiDaemon::instance().stop();
        return std::unexpected(asio::error::fault);
    }

    return I2pd(std::make_unique<InnerLib>(
        log_path
    ));
}

} // namespace
