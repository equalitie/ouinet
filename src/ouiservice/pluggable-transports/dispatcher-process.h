#pragma once

#include <vector>

#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/optional.hpp>
#include <boost/process/async_pipe.hpp>
#include <boost/process/child.hpp>

#include "../../namespaces.h"
#include "../../util/signal.h"

namespace ouinet {
namespace ouiservice {
namespace pt {

class DispatcherProcess
{
    public:
    DispatcherProcess(
        asio::io_service& ios,
        std::string command,
        std::vector<std::string> command_line_arguments,
        boost::optional<std::string> state_directory
    );
    ~DispatcherProcess();

    protected:
    void start_process(
        std::map<std::string, std::string> environment,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    );
    void stop_process();

    virtual void process_output_line(
        std::string command,
        std::vector<std::string> args,
        sys::error_code& ec,
        bool& initialized
    );

    protected:
    asio::io_service& _ios;
    std::string _command;
    std::vector<std::string> _command_line_arguments;
    boost::optional<std::string> _state_directory;

    std::unique_ptr<boost::process::child> _process;
    std::unique_ptr<boost::process::async_pipe> _standard_input;
    std::unique_ptr<Signal<void()>> _process_exit;

    Signal<void()> _stop_signal;
};

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
