#include "client-process.h"
#include "util.h"

namespace ouinet {
namespace ouiservice {
namespace pt {

ClientProcess::ClientProcess(
    asio::io_context& ioc,
    std::string command,
    std::vector<std::string> command_line_arguments,
    std::string transport_name,
    boost::optional<std::string> state_directory
):
    DispatcherProcess(ioc, command, command_line_arguments, state_directory),
    _transport_name(transport_name),
    _transport_initialized(false)
{
}

void ClientProcess::start(asio::yield_context yield, Signal<void()>& cancel_signal)
{
    std::map<std::string, std::string> environment;
    environment["TOR_PT_CLIENT_TRANSPORTS"] = _transport_name;

    start_process(environment, yield, cancel_signal);
}

void ClientProcess::stop()
{
    stop_process();
}

void ClientProcess::process_output_line(
    std::string command,
    std::vector<std::string> args,
    sys::error_code& ec,
    bool& initialized
) {
    if (command == "PROXY") {
        ec = asio::error::fault;
    } else if (command == "PROXY-ERROR") {
        ec = asio::error::fault;
    } else if (command == "CMETHOD") {
        if (args.size() < 3) {
            ec = asio::error::fault;
            return;
        }
        if (args[0] != _transport_name) {
            return;
        }
        if (args[1] == "socks5") {
            _connection_method = Socks5Connection;
        } else if (args[1] == "transparent-TCP") {
            _connection_method = TransparentConnection;
        } else {
            ec = asio::error::fault;
            return;
        }
        boost::optional<asio::ip::tcp::endpoint> endpoint = parse_endpoint(args[2]);
        if (!endpoint) {
            ec = asio::error::fault;
            return;
        }
        _endpoint = *endpoint;
        _transport_initialized = true;
    } else if (command == "CMETHOD-ERROR") {
        ec = asio::error::fault;
    } else if (command == "CMETHODS") {
        if (args.size() != 1 || args[0] != "DONE") {
            ec = asio::error::fault;
            return;
        }
        if (_transport_initialized) {
            initialized = true;
        } else {
            ec = asio::error::operation_not_supported;
        }
    } else {
        DispatcherProcess::process_output_line(command, args, ec, initialized);
    }
}

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
