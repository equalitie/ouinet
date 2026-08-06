#include "i2pd.h"

namespace ouinet {

std::vector<std::string> I2pd::Config::to_vector() const {
    return std::vector<std::string>{
        "--datadir", (i2pd_root_dir / "datadir").string(),
        "--tunconf", (i2pd_root_dir / "tunnels.conf").string(),
        "--certsdir", (i2pd_root_dir / "certificates").string(),
        "--httpproxy.enabled=0",
        "--socksproxy.enabled=0",
        "--sam.enabled=1",
        "--http.enabled=0", // web console
        "--upnp.enabled=0", // default is disabled, should we enable?
        "--loglevel=warn", // warn is default, choices are: debug, info, warn, error, none
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
