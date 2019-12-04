#include "server-process.h"
#include "util.h"

namespace ouinet {
namespace ouiservice {
namespace pt {

ServerProcess::ServerProcess(
    asio::io_context& ioc,
    std::string command,
    std::vector<std::string> command_line_arguments,
    std::string transport_name,
    boost::optional<asio::ip::tcp::endpoint> bind_address,
    asio::ip::tcp::endpoint destination_address,
    std::map<std::string, std::string> transport_options,
    boost::optional<std::string> state_directory
):
    DispatcherProcess(ioc, command, command_line_arguments, state_directory),
    _transport_name(transport_name),
    _bind_address(bind_address),
    _destination_address(destination_address),
    _transport_options(transport_options),
    _transport_initialized(false)
{
}

void ServerProcess::start(asio::yield_context yield, Signal<void()>& cancel_signal)
{
    std::map<std::string, std::string> environment;
    environment["TOR_PT_SERVER_TRANSPORTS"] = _transport_name;
    if (!_transport_options.empty()) {
        std::string transport_options;
        for (auto i : _transport_options) {
            if (!transport_options.empty()) {
                transport_options += ";";
            }
            transport_options += _transport_name;
            transport_options += ":";
            transport_options += string_escape(i.first, ":;=");
            transport_options += "=";
            transport_options += string_escape(i.second, ":;=");
        }
        environment["TOR_PT_SERVER_TRANSPORT_OPTIONS"] = transport_options;
    }
    if (_bind_address) {
        std::string bind_address;
        bind_address += _transport_name;
        bind_address += "-";
        bind_address += format_endpoint(*_bind_address);
        environment["TOR_PT_SERVER_BINDADDR"] = bind_address;
    }
    environment["TOR_PT_ORPORT"] = format_endpoint(_destination_address);

    start_process(environment, yield, cancel_signal);
}

void ServerProcess::stop()
{
    stop_process();
}

void ServerProcess::process_output_line(
    std::string command,
    std::vector<std::string> args,
    sys::error_code& ec,
    bool& initialized
) {
    if (command == "SMETHOD") {
        if (args.size() < 2) {
            ec = asio::error::fault;
            return;
        }
        if (args[0] != _transport_name) {
            return;
        }
        boost::optional<asio::ip::tcp::endpoint> endpoint = parse_endpoint(args[1]);
        if (!endpoint) {
            ec = asio::error::fault;
            return;
        }
        _listening_endpoint = *endpoint;

        for (size_t i = 2; i < args.size(); i++) {
            std::string arg = args[i];
            if (arg.substr(0, 5) == "ARGS:") {
                _connection_arguments = arg.substr(5);
            }
        }

        _transport_initialized = true;
    } else if (command == "SMETHOD-ERROR") {
        ec = asio::error::fault;
    } else if (command == "SMETHODS") {
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
