#include "i2pd.h"
#include "logger.h"

namespace ouinet {

const char* log_level_to_str(I2pd::LogLevel log_level) {
    switch (log_level) {
        case I2pd::LogLevel::debug: return "debug";
        case I2pd::LogLevel::info: return "info";
        case I2pd::LogLevel::warn: return "warn";
        case I2pd::LogLevel::error: return "error";
        case I2pd::LogLevel::none: return "none";
        default:
            LOG_WARN("Invalid I2pd log level enum ", static_cast<int>(log_level));
            return "none";
    }
}

std::vector<std::string> I2pd::Config::to_vector() const {
    using namespace std::string_literals;

    return std::vector<std::string>{
        "--datadir", (i2pd_root_dir / "datadir").string(),
        "--tunconf", (i2pd_root_dir / "tunnels.conf").string(),
        "--certsdir", (i2pd_root_dir / "certificates").string(),
        "--httpproxy.enabled=0",
        "--socksproxy.enabled=0",
        "--sam.enabled=1",
        "--http.enabled=0", // web console
        "--upnp.enabled=0", // default is disabled, should we enable?
        "--loglevel="s + log_level_to_str(log_level),
    }; 
}

#ifndef OUINET_WITH_I2PD_EXE

bool I2pd::is_start_exe_implemented() { return false; }

std::expected<I2pd, sys::error_code>
I2pd::start_exe(fs::path, I2pd::Config, asio::any_io_executor, util::LogPath) {
    return std::unexpected(make_error_code(sys::errc::not_supported));
}

#endif

#ifndef OUINET_WITH_I2PD_LIB

bool I2pd::is_start_lib_implemented() { return false; }

std::expected<I2pd, sys::error_code>
I2pd::start_lib(I2pd::Config, util::LogPath) {
    return std::unexpected(make_error_code(sys::errc::not_supported));
}

#endif

} // namespace
