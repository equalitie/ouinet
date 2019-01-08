#pragma once

#include <boost/asio/ip/tcp.hpp>

#include "dispatcher-process.h"

namespace ouinet {
namespace ouiservice {
namespace pt {

class ClientProcess : public DispatcherProcess
{
    public:
    enum ConnectionMethod {
        Socks5Connection,
        TransparentConnection
    };

    public:
    ClientProcess(
        asio::io_service& ios,
        std::string command,
        std::vector<std::string> command_line_arguments,
        std::string transport_name,
        boost::optional<std::string> state_directory
    );

    void start(asio::yield_context yield, Signal<void()>& cancel_signal);
    void stop();

    asio::ip::tcp::endpoint endpoint() const { return _endpoint; }
    ConnectionMethod connection_method() const { return _connection_method; }

    protected:
    void process_output_line(
        std::string command,
        std::vector<std::string> args,
        sys::error_code& ec,
        bool& initialized
    ) override;

    protected:
    std::string _transport_name;

    bool _transport_initialized;
    asio::ip::tcp::endpoint _endpoint;
    ConnectionMethod _connection_method;
};

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
