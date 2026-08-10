#pragma once

#include "namespaces.h"
#include "util/log_path.h"
#include "api.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/system/error_code.hpp>
#include <boost/filesystem/path.hpp>
#include <expected>

namespace ouinet {

class Async;

// Class for controlling the `i2pd` executable
class OUINET_I2P_API I2pd {
public:
    [[nodiscard]]
    static std::expected<I2pd, sys::error_code> start(fs::path i2pd_binary_path, fs::path root_dir, asio::any_io_executor, util::LogPath);

    I2pd(I2pd&&);
    I2pd& operator=(I2pd&&);

    ~I2pd();

private:
    struct Inner;

    I2pd(std::unique_ptr<Inner>);

private:
    std::unique_ptr<Inner> _inner;
};

} // namespace
