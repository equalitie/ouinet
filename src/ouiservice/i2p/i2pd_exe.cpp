#include "i2pd.h"
#include "util/async.h"
#include "util/str.h"
#include "task.h"
#include "logger.h"
#include "util/log_path.h"

#include <boost/process.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/filesystem/path.hpp>

#include <streambuf>
#include <iostream>

#ifdef __unix__
#   include <linux/prctl.h>  /* Definition of PR_* constants */
#   include <sys/prctl.h>
#endif

#ifndef OUINET_WITH_I2PD_EXE
#error "Compiling i2pd_exe.cpp but OUINET_WITH_I2PD_EXE is not defined"
#endif

namespace ouinet {

namespace bp = boost::process::v2;

bool I2pd::is_start_exe_implemented() { return true; }

// NOTE: iOS does not allow starting processes (macOS does).
struct I2pd::InnerExe : I2pd::InnerBase {
    bp::process _process;
    asio::readable_pipe _out;
    util::LogPath _log_path;
    Cancel _cancel;

    InnerExe(bp::process process, asio::readable_pipe out, util::LogPath log_path):
        _process(std::move(process)),
        _out(std::move(out)),
        _log_path(std::move(log_path))
    {
        task::spawn_detached(process.get_executor(), [this, cancel = _cancel] (asio::yield_context y_) mutable {
            Async yield(y_, cancel);

            auto slot = cancel.connect([&] { if (_out.is_open()) _out.close(); });

            asio::streambuf buffer;
            std::string output;
            std::string line;

            auto err_trace = _log_path.tag("i2pd");
            auto log_trace = _log_path.tag("i2pd").tag("log");

            while (true) {
                auto size_r = asio::async_read_until(_out, buffer, '\n', yield);

                if (!size_r.has_value()) {
                    LOG_DEBUG(err_trace, " ", size_r.error().message());
                    break;
                }

                std::istream is(&buffer);
                std::string line;
                std::getline(is, line);

                LOG_DEBUG(log_trace, " ", line);
            }
        });
    }

    ~InnerExe() {
        _cancel();
        _process.terminate();
    }
};

// Ensure the process is killed when the main process dies.
// https://github.com/boostorg/process/issues/454
// TODO: Windows, MacOS (iOS doesn't support child processes)
#   ifdef __unix__
struct ExecHandler {
    sys::error_code on_exec_setup(
            bp::posix::default_launcher & /*launcher*/,
            const fs::path & /* exe*/,
            const char * const * /* argv*/) {
        (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
        return sys::error_code();
    }
};
#   endif

// static
std::expected<I2pd, sys::error_code> I2pd::start_exe(
        fs::path i2pd_binary_path,
        I2pd::Config config,
        asio::any_io_executor exec,
        util::LogPath log_path) {
    LOG_DEBUG(log_path, " Starting I2P daemon (process)");
    asio::readable_pipe out{exec};

    try {
        bp::process proc(
            exec,
            i2pd_binary_path.string(),
            config.to_vector(),
            bp::process_stdio{
                {}, // stdin
                out, // stdout
                out // stdcerr
            }
#           ifdef __unix__
            , ExecHandler{});
#           else
          );
#           endif

        return I2pd(std::make_unique<InnerExe>(
                std::move(proc),
                std::move(out),
                std::move(log_path)
            ));
    }
    catch (const sys::system_error& e) {
        LOG_WARN(log_path, " Failed to start `i2pd`, system_error: ", e.what());
        return std::unexpected(e.code());
    }
    catch (const std::exception& e) {
        LOG_WARN(log_path, " Failed to start `i2pd`, std::exception: ", e.what());
        return std::unexpected(asio::error::fault);
    }
    catch (...) {
        LOG_WARN(log_path, " Failed to start `i2pd`");
        return std::unexpected(asio::error::fault);
    }
}

} // namespace
