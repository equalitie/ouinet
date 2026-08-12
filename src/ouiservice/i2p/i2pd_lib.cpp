#include "i2pd.h"
#include "ClientContext.h"
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
    asio::ip::tcp::endpoint sam_ep;
    util::LogPath log_path;

    InnerLib(asio::ip::tcp::endpoint sam_ep, util::LogPath log_path):
        sam_ep(sam_ep),
        log_path(std::move(log_path))
    {}

    asio::ip::tcp::endpoint sam_endpoint() const override {
        return sam_ep;
    }

    ~InnerLib() {
        OUI_LOG_DEBUG(log_path, " Stopping I2P daemon");
        OuiDaemon::instance().stop();
    }
};

std::expected<I2pd, sys::error_code>
I2pd::start_lib(I2pd::Config config, util::LogPath log_path) {
    OUI_LOG_DEBUG(log_path, " Starting I2P daemon (library)");

    auto config_vec = config.to_vector();

    std::vector<const char*> args;

    args.push_back("i2pd");

    std::transform(
            config_vec.begin(),
            config_vec.end(),
            std::back_inserter(args),
            [] (const std::string& str) { return str.c_str(); });

    if (!OuiDaemon::instance().init(args.size(), (char**) args.data())) {
        OUI_LOG_WARN(log_path, " Failed to initialize I2P daemon");
        return std::unexpected(asio::error::fault);
    }

    if (!OuiDaemon::instance().start()) {
        OUI_LOG_WARN(log_path, " Failed to start I2P daemon");
        OuiDaemon::instance().stop();
        return std::unexpected(asio::error::fault);
    }

    auto sam_bridge = i2p::client::context.GetSAMBridge();

    if (!sam_bridge) {
        OUI_LOG_WARN(log_path, " Failed to obtain SAMBridge");
        return std::unexpected(asio::error::fault);
    }

    sys::error_code ec;
    auto ep = sam_bridge->GetAcceptorEndpoint(ec);

    if (ec) {
        OUI_LOG_WARN(log_path, " Failed to obtain endpoint of SAMBridge: ", ec);
        return std::unexpected(ec);
    }

    return I2pd(std::make_unique<InnerLib>(
        ep,
        log_path
    ));
}

} // namespace
