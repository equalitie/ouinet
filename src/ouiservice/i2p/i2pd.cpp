#include "i2pd.h"
#include "util/async.h"
#include "util/str.h"
#include "task.h"

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

namespace ouinet {

namespace bp = boost::process::v2;

struct I2pd::Inner {
    bp::process _process;
    asio::readable_pipe _out;
    util::LogPath _log_path;
    Cancel _cancel;

    Inner(bp::process process, asio::readable_pipe out, util::LogPath log_path):
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

            while (true) {
                auto size_r = asio::async_read_until(_out, buffer, '\n', yield);

                if (!size_r.has_value()) {
                    std::cerr << _log_path << " " << size_r.error().message() << "\n";
                    break;
                }

                std::istream is(&buffer);
                std::string line;
                std::getline(is, line);

                std::cerr << _log_path << " " << line << "\n";
            }
        });
    }

    ~Inner() {
        _cancel();
        _process.terminate();
    }
};

I2pd::I2pd(std::unique_ptr<Inner> inner):
    _inner(std::move(inner))
{}

// Ensure the process is killed when the main process dies.
// https://github.com/boostorg/process/issues/454
// TODO: Windows, MacOS, iOS
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
std::expected<I2pd, sys::error_code> I2pd::start(fs::path i2pd_binary_path, fs::path root_dir_path, asio::any_io_executor exec, util::LogPath log_path) {
    asio::readable_pipe out{exec};

    bp::process proc(exec, i2pd_binary_path.string(), {
            "--datadir", (root_dir_path / "datadir").string(),
            "--tunconf", (root_dir_path / "tunnels.conf").string(),
            "--certsdir", (root_dir_path / "certificates").string(),
            "--httpproxy.enabled=0",
            "--socksproxy.enabled=0",
            "--sam.enabled=1",
            "--http.enabled=0", // web console
            "--upnp.enabled=0", // default is disabled, should we enable?
            "--loglevel=warn", // warn is default, choices are: debug, info, warn, error, none
        }, bp::process_stdio{
            {}, // stdin
            out, // stdout
            out // stdcerr
        }
#ifdef __unix__
        , ExecHandler{});
#else
        );
#endif

    return I2pd(std::make_unique<Inner>(
            std::move(proc),
            std::move(out),
            std::move(log_path)
        ));
}

I2pd::I2pd(I2pd&&) = default;
I2pd& I2pd::operator=(I2pd&& other) = default;
I2pd::~I2pd() {}

} // namespace
