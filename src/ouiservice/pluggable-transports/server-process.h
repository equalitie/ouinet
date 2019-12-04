#pragma once

#include <boost/asio/ip/tcp.hpp>

#include "dispatcher-process.h"

namespace ouinet {
namespace ouiservice {
namespace pt {

class ServerProcess : public DispatcherProcess
{
    public:
    ServerProcess(
        asio::io_context&,
        std::string command,
        std::vector<std::string> command_line_arguments,
        std::string transport_name,
        boost::optional<asio::ip::tcp::endpoint> bind_address,
        asio::ip::tcp::endpoint destination_address,
        std::map<std::string, std::string> transport_options,
        boost::optional<std::string> state_directory
    );

    void start(asio::yield_context yield, Signal<void()>& cancel_signal);
    void stop();

    asio::ip::tcp::endpoint listening_endpoint() const { return _listening_endpoint; }
    std::string connection_arguments() const { return _connection_arguments; }

    protected:
    void process_output_line(
        std::string command,
        std::vector<std::string> args,
        sys::error_code& ec,
        bool& initialized
    ) override;

    protected:
    std::string _transport_name;
    boost::optional<asio::ip::tcp::endpoint> _bind_address;
    asio::ip::tcp::endpoint _destination_address;
    std::map<std::string, std::string> _transport_options;

    bool _transport_initialized;
    asio::ip::tcp::endpoint _listening_endpoint;
    std::string _connection_arguments;
};

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
