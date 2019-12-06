#include "dispatcher-process.h"
#include "../../or_throw.h"
#include "../../util/condition_variable.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/process.hpp>
#include <system_error>

namespace ouinet {
namespace ouiservice {
namespace pt {

static void parse_output_line(std::string line, std::string& command, std::vector<std::string>& args)
{
    size_t pos = line.find(' ');
    if (pos == std::string::npos) {
        command = line;
        return;
    } else {
        command = line.substr(0, pos);
        line = line.substr(pos + 1);
    }

    while (!line.empty()) {
        while (line[0] == ' ') {
            line.erase(line.begin());
        }
        if (line.empty()) {
            break;
        }
        pos = line.find(' ');
        if (pos == std::string::npos) {
            args.push_back(line);
            break;
        } else {
            args.push_back(line.substr(0, pos));
            line = line.substr(pos + 1);
        }
    }
}

DispatcherProcess::DispatcherProcess(
    asio::io_context& ioc,
    std::string command,
    std::vector<std::string> command_line_arguments,
    boost::optional<std::string> state_directory
):
    _ioc(ioc),
    _command(command),
    _command_line_arguments(command_line_arguments),
    _state_directory(state_directory)
{
}

DispatcherProcess::~DispatcherProcess()
{
    stop_process();
}

void DispatcherProcess::start_process(
    std::map<std::string, std::string> environment,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    assert(!_process);

    boost::process::environment env = boost::this_process::environment();
    std::vector<std::string> to_remove;
    for (auto i : env) {
        if (i.get_name().substr(0, 7) == "TOR_PT_") {
            to_remove.push_back(i.get_name());
        }
    }
    for (auto i : to_remove) {
        env.erase(i);
    }
    env["TOR_PT_MANAGED_TRANSPORT_VER"] = "1";
    env["TOR_PT_EXIT_ON_STDIN_CLOSE"] = "1";
    if (_state_directory) {
        env["TOR_PT_STATE_LOCATION"] = *_state_directory;
    }
    for (auto i : environment) {
        env[i.first] = i.second;
    }

    _standard_input = std::make_unique<boost::process::async_pipe>(_ioc);
    auto standard_output = std::make_unique<boost::process::async_pipe>(_ioc);
    _process_exit = std::make_unique<Signal<void()>>();

    std::error_code error_code;
    _process = std::make_unique<boost::process::child>(
        _command,
        boost::process::args(_command_line_arguments),
        boost::process::env(env),
        boost::process::std_in < *_standard_input,
        boost::process::std_out > *standard_output,
        boost::process::std_err > boost::process::null,
        boost::process::error(error_code),
        boost::process::on_exit([
            signal = _process_exit.get()
        ] (int exit, const std::error_code& error_code) {
            (*signal)();
        }),
        _ioc
    );

    if (error_code) {
        return or_throw(yield, boost::system::errc::make_error_code(
            static_cast<boost::system::errc::errc_t>(error_code.value())
        ));
    }



    /*
     * Start the output processing coroutine, and wait for it to signal
     * successful initialization.
     * Abort on:
     * - cancellation;
     * - object destruction;
     * - timeout.
     */

    struct InitializationStatus {
        ConditionVariable stop_condition;
        boost::optional<sys::error_code> ec;
        InitializationStatus(asio::io_context& ioc):
            stop_condition(ioc.get_executor()),
            ec(boost::none)
        {}
    };
    std::shared_ptr<InitializationStatus> initialization =
        std::make_shared<InitializationStatus>(_ioc);

    asio::steady_timer timeout_timer(_ioc);
    timeout_timer.expires_from_now(std::chrono::seconds(15));
    timeout_timer.async_wait([initialization] (const sys::error_code&) {
        if (!initialization->ec) {
            initialization->ec = asio::error::timed_out;
        }
        initialization->stop_condition.notify();
    });

    auto cancelled = cancel_signal.connect([&] {
        initialization->ec = asio::error::operation_aborted;
        timeout_timer.cancel();
    });

    auto stopped = _stop_signal.connect([&] {
        initialization->ec = asio::error::operation_aborted;
        timeout_timer.cancel();
    });

    asio::spawn(_ioc, [
        this,
        standard_output = std::move(standard_output),
        initialization
    ] (asio::yield_context yield) {
        std::string output_buffer;
        /*
         * The output processing coroutine can finish initialization in three forms:
         * - successful initialization;
         * - error reported by process_output_line;
         * - EOF.
         *
         * The output processing coroutine keeps going until it encounters an EOF.
         * It is not aborted by the DispatcherProcess under any circumstance;
         * rather, the DispatcherProcess kills the process, which will EOF the
         * pipe as soon as pending output is processed. After completion,
         * either successful or not, further output is ignored.
         */

        /*
         * Reading from an async_pipe doesn't work well in boost 1.67. The
         * source and sink endpoints are standard boost::asio components, and
         * they don't cause problems, so using them works around the issue.
         */
        auto standard_output_source = std::move(*standard_output).source();

        while (true) {
            char buffer[4096];
            sys::error_code ec;

            size_t read = standard_output_source.async_read_some(
                asio::mutable_buffers_1(buffer, sizeof(buffer)),
                yield[ec]
            );

            if (ec || !read) {
                break;
            }

            if (initialization->ec) {
                continue;
            }

            output_buffer.append(buffer, buffer + read);

            size_t pos;
            while ((pos = output_buffer.find('\n')) != std::string::npos) {
                std::string line = output_buffer.substr(0, pos);
                output_buffer = output_buffer.substr(pos + 1);

                std::string command;
                std::vector<std::string> args;
                parse_output_line(line, command, args);

                bool initialized = false;
                process_output_line(command, args, ec, initialized);
                if (ec || initialized) {
                    assert(!initialization->ec);
                    initialization->ec = ec;
                    initialization->stop_condition.notify();
                    output_buffer.clear();
                    break;
                }
            }
        }

        standard_output_source.close();

        if (!initialization->ec) {
            initialization->ec = asio::error::broken_pipe;
            initialization->stop_condition.notify();
        }
    });

    initialization->stop_condition.wait(yield);

    assert(initialization->ec);
    sys::error_code ec = *initialization->ec;
    timeout_timer.cancel();

    /*
     * If stopped() has been called, the process has already been cleaned up,
     * and $this may be gone, so we abort before calling stop().
     */
    if (stopped) {
        return or_throw(yield, ec);
    }

    if (ec) {
        stop_process();
        return or_throw(yield, ec);
    }
}

void DispatcherProcess::stop_process()
{
    if (!_process) {
        return;
    }

    auto process = std::move(_process);
    auto standard_input = std::move(_standard_input);
    auto process_exit = std::move(_process_exit);
    auto& ioc = _ioc;
    _stop_signal();

    asio::spawn(_ioc, [
        &ioc,
        process = std::move(process),
        standard_input = std::move(standard_input),
        process_exit = std::move(process_exit)
    ] (asio::yield_context yield) {
        standard_input->close();

        /*
         * Closing the standard input triggers the process to quit.
         * Wait for process exit or a timeout.
         */
        asio::steady_timer timeout_timer(ioc);
        timeout_timer.expires_from_now(std::chrono::seconds(5));

        auto exited = process_exit->connect([&] {
            timeout_timer.cancel();
        });

        sys::error_code ec;
        timeout_timer.async_wait(yield[ec]);

        if (process->running()) {
            process->terminate();
        }
    });
}

void DispatcherProcess::process_output_line(
    std::string command,
    std::vector<std::string> args,
    sys::error_code& ec,
    bool& initialized
) {
    if (command == "VERSION-ERROR") {
        ec = asio::error::operation_not_supported;
    } else if (command == "ENV-ERROR") {
        ec = asio::error::operation_not_supported;
    }
}

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
