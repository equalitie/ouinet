#pragma once

#include "namespaces.h"
#include "api.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/filesystem/path.hpp>
#include <expected>
#include <vector>
#include <string>

namespace ouinet {

class Async;
namespace util { class LogPath; }

// Class for controlling the `i2pd` executable/library
class OUINET_I2P_API I2pd {
public:
    class Config {
    public:
        Config(fs::path i2pd_root_dir):
            i2pd_root_dir(std::move(i2pd_root_dir))
        {}
    
        std::vector<std::string> to_vector() const;
    
    private:
        fs::path i2pd_root_dir;
    };

    // If these return `false` then the corresponding `start_*` functions
    // declared below will return immediatelly with an error.
    static bool is_start_lib_implemented();
    static bool is_start_exe_implemented();

    // Start i2pd using an external executable in a new process.
    [[nodiscard]]
    static
    std::expected<I2pd, sys::error_code>
    start_exe(fs::path i2pd_binary_path, Config, asio::any_io_executor, util::LogPath);

    // Start i2pd through i2pd library compiled into ouinet.
    [[nodiscard]]
    static
    std::expected<I2pd, sys::error_code>
    start_lib(Config, util::LogPath);

private:
    struct InnerLib;
    struct InnerExe;
    struct InnerBase { virtual ~InnerBase() = default; };

    I2pd(std::unique_ptr<InnerBase> inner) : _inner(std::move(inner)) {}

    std::unique_ptr<InnerBase> _inner;
};

} // namespace
