#include "i2pd.h"
#include "util/async.h"
#include "util/log_path.h"
#include "util/str.h"
#include "util/overloaded.h"
#include "logger.h"

namespace ouinet {

std::vector<std::string> I2pd::Config::to_vector(I2pd::Type type) const {
    using namespace std::string_literals;

    std::vector<std::string> ret{
        "--datadir", (i2pd_root_dir / "datadir").string(),
        "--tunconf", (i2pd_root_dir / "tunnels.conf").string(),
        "--certsdir", (i2pd_root_dir / "certificates").string(),
        "--httpproxy.enabled=0",
        "--socksproxy.enabled=0",
        "--sam.enabled=1",
        "--sam.port="s + util::str(sam_port),
        "--http.enabled=0", // web console
        "--upnp.enabled=0", // default is disabled, should we enable?
    }; 

    // Possible log level values: "debug", "info", "warn", "error", "none".
    //
    // Note 1: `Type::Exe` needs higher loglevel because we read SAM endpoint
    //          from the log.
    // Note 2: `Type::Exe` implementation reads the log but doesn't output it
    //          by default, edit i2pd_exe.cpp to see it.
    type.visit(overloaded {
        [&ret] (Type::Exe) { ret.push_back("--loglevel=info"s); },
        [&ret] (Type::Lib) { ret.push_back("--loglevel=none"s); }
    });

    return ret;
}

asio::ip::tcp::endpoint I2pd::sam_endpoint() const {
    return _inner->sam_endpoint();
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
