#include "i2pd.h"
#include "util/log_path.h"
#include "util/str.h"
#include "logger.h"

namespace ouinet {

enum class I2pdLogLevel { debug, info, warn, error, none };

// I used `none` here because it's quite verbose and doesn't seem to
// provide much information. Normally the default is `warn`.
static constexpr I2pdLogLevel log_level = I2pdLogLevel::info;

static const char* log_level_to_str(I2pdLogLevel log_level) {
    switch (log_level) {
        case I2pdLogLevel::debug: return "debug";
        case I2pdLogLevel::info: return "info";
        case I2pdLogLevel::warn: return "warn";
        case I2pdLogLevel::error: return "error";
        case I2pdLogLevel::none: return "none";
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
        "--sam.port="s + util::str(sam_port),
        "--http.enabled=0", // web console
        "--upnp.enabled=0", // default is disabled, should we enable?
        "--loglevel="s + log_level_to_str(log_level),
    }; 
}

#ifndef OUINET_WITH_I2PD_EXE

bool I2pd::is_start_exe_implemented() { return false; }

std::expected<I2pd, sys::error_code>
I2pd::start_exe(fs::path, I2pd::Config, Async) {
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
