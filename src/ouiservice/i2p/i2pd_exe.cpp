#include "i2pd.h"
#include "sam.h"
#include "util/async.h"
#include "util/str.h"
#include "task.h"
#include "logger.h"
#include "util/log_path.h"
#include "util/condition_variable.h"
#include "parse/endpoint.h"

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

struct Line {
    util::LogPath trace;
    std::string text;
};

template<class T> class Tail {
private:
    // Points to the next position to insert to
    size_t _next = 0;
    size_t _size = 0;
    std::vector<T> _lines;

public:
    Tail(size_t max_size) : _lines(max_size) {}

    size_t size() const { return _size; }

    void insert(T line) {
        if (_lines.size() == 0) return;

        _lines[_next++] = std::move(line);
        if (_next == _lines.size()) _next = 0;

        if (_size < _lines.size()) ++_size;
    }

    const T& operator[](size_t i) const {
        assert(_size > 0);
        assert(_size <= _lines.size());
        assert(_size >= _next);
        size_t first = _lines.size() - (_size - _next);
        return _lines[(first + i) % _lines.size()];
    }
};

// NOTE: iOS does not allow starting processes (macOS does).
struct I2pd::InnerExe : I2pd::InnerBase {
    bp::process _process;
    asio::readable_pipe _stdcout;
    asio::readable_pipe _stdcerr;
    util::LogPath _log_path;
    Cancel _cancel;
    std::optional<std::expected<asio::ip::tcp::endpoint, sys::error_code>> _sam_ep;
    ConditionVariable _cv;
    Tail<Line> tail;

    InnerExe(bp::process process, asio::readable_pipe stdcout, asio::readable_pipe stdcerr, util::LogPath log_path):
        _process(std::move(process)),
        _stdcout(std::move(stdcout)),
        _stdcerr(std::move(stdcerr)),
        _log_path(std::move(log_path)),
        _cv(process.get_executor()),
        tail(8)
    {
        start_reading_pipe(_stdcout, "out");
        start_reading_pipe(_stdcerr, "err");
    }

    void start_reading_pipe(asio::readable_pipe& pipe, const char* tag) {
        task::spawn_detached(_process.get_executor(), [this, &pipe, tag, cancel = _cancel] (asio::yield_context y_) mutable {
            Async yield(y_, cancel);

            auto slot = cancel.connect([&] { if (pipe.is_open()) pipe.close(); });

            asio::streambuf buffer;
            std::string output;
            std::string line;

            auto log_trace = _log_path.tag(tag);

            while (true) {
                auto size_r = asio::async_read_until(pipe, buffer, '\n', yield);

                if (!size_r.has_value()) {
                    LOG_DEBUG(_log_path, " ", size_r.error().message());
                    print_tail();
                    break;
                }

                std::istream is(&buffer);
                std::string line;
                std::getline(is, line);

                process_line(log_trace, std::move(line));
            }
        });
    }

    void process_line(const util::LogPath& trace, std::string line) {
        // Uncomment to see full log from i2pd
        //LOG_DEBUG(trace, " ", line);

        static const std::string_view sam_bound = "SAM: Bridge bound to TCP endpoint ";
        static const std::string_view sam_bound_fail = "Clients: Exception in SAM bridge:";

        auto pos = line.find(sam_bound);

        if (pos != std::string::npos) {
            std::string_view ep_str(line.begin() + pos + sam_bound.size(), line.end());

            auto sam_ep_opt = parse::endpoint<asio::ip::tcp>(boost::string_view(ep_str.begin(), ep_str.size()));

            if (sam_ep_opt) {
                _sam_ep = *sam_ep_opt;
            }
            else {
                _sam_ep = std::unexpected(asio::error::invalid_argument);
            }
            _cv.notify();
        }
        else {
            pos = line.find(sam_bound_fail);

            if (pos != std::string::npos) {
                _sam_ep = std::unexpected(asio::error::fault);
                _cv.notify();
            }
        }

        tail.insert(Line { trace, std::move(line) });
    }

    asio::ip::tcp::endpoint sam_endpoint() const override {
        if (!_sam_ep || !*_sam_ep) {
            std::unreachable();
        }
        return **_sam_ep;
    }

    std::expected<asio::ip::tcp::endpoint, sys::error_code>
    wait_for_sam_endpoint_log_line(Async yield) {
        while (true) {
            if (_sam_ep) {
                if (*_sam_ep) {
                    return *_sam_ep;
                }
                else {
                    return std::unexpected(_sam_ep->error());
                }
            }
            if (auto r = _cv.wait(yield); !r) {
                return std::unexpected(r.error());
            }
        }
    }

    void print_tail() {
        LOG_DEBUG(_log_path, " Tail from closed i2pd:");
        for (size_t i = 0; i < tail.size(); ++i) {
            LOG_DEBUG(tail[i].trace, " ", tail[i].text);
        }
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
        Async yield) {
    auto log_path = yield.log_path();
    auto exec = yield.get_executor();

    LOG_DEBUG(log_path, " Starting I2P daemon (process)");
    asio::readable_pipe stdcout{exec};
    asio::readable_pipe stdcerr{exec};

    std::unique_ptr<InnerExe> inner;

    try {
        bp::process proc(
            exec,
            i2pd_binary_path.string(),
            config.to_vector(I2pd::Type::Exe{}),
            bp::process_stdio{
                {}, // stdin
                stdcout, // stdout
                stdcerr // stdcerr
            }
          );

        inner = std::make_unique<InnerExe>(
                std::move(proc),
                std::move(stdcout),
                std::move(stdcerr),
                std::move(log_path)
            );
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

    if (auto r = inner->wait_for_sam_endpoint_log_line(yield); !r) {
        LOG_WARN(log_path, " Failed to read `i2pd` SAM endpoint");
        inner->print_tail();
        return std::unexpected(r.error());
    }

    return I2pd(std::move(inner));
}

} // namespace
