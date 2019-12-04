#pragma once

#include <boost/asio/ip/tcp.hpp>
#include "../../ouiservice.h"
#include "../../util/condition_variable.h"

namespace ouinet {
namespace ouiservice {
namespace pt {

class ClientProcess;
class ServerProcess;

class PtOuiServiceServer : public OuiServiceImplementationServer
{
    public:
    PtOuiServiceServer(asio::io_context&);
    ~PtOuiServiceServer();

    void start_listen(asio::yield_context yield) final;
    void stop_listen() final;

    GenericStream accept(asio::yield_context yield) final;

    /*
     * Wait for the next start_listen() call to complete.
     * Reports the same error condition as start_listen() itself.
     */
    void wait_for_running(asio::yield_context yield);
    std::string connection_arguments() const;

    protected:
    virtual std::unique_ptr<ServerProcess> start_server_process(
        asio::io_context&,
        asio::ip::tcp::endpoint destination_endpoint,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) = 0;

    private:
    asio::io_context& _ioc;

    asio::ip::tcp::acceptor _acceptor;
    std::unique_ptr<pt::ServerProcess> _server_process;
    ConditionVariable _start_listen_condition;
};

class PtOuiServiceClient : public OuiServiceImplementationClient
{
    public:
    PtOuiServiceClient(asio::io_context&);
    ~PtOuiServiceClient();

    void start(asio::yield_context yield) final;
    void stop() final;

    GenericStream connect(
        asio::yield_context yield,
        Signal<void()>& cancel
    ) final;

    protected:
    virtual std::unique_ptr<ClientProcess> start_client_process(
        asio::io_context&,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) = 0;
    virtual asio::ip::tcp::socket connect_through_transport(
        const asio::executor&,
        asio::ip::tcp::endpoint transport_endpoint,
        std::string& remote_endpoint_string,
        asio::yield_context yield,
        Signal<void()>& cancel_signal
    ) = 0;

    private:
    asio::io_context& _ioc;

    std::unique_ptr<pt::ClientProcess> _client_process;
};

} // pt namespace
} // ouiservice namespace
} // ouinet namespace
