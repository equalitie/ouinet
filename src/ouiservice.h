#pragma once

#include <list>
#include <memory>
#include <vector>

#include <boost/asio/spawn.hpp>

#include "generic_stream.h"
#include "endpoint.h"
#include "util/condition_variable.h"
#include "util/signal.h"

#include <expected>

namespace ouinet {

class Async;

class OuiServiceImplementationServer
{
    public:
    virtual ~OuiServiceImplementationServer() {}

    [[nodiscard]]
    virtual sys::error_code start_listen(Async) = 0;
    virtual void stop_listen() = 0;

    [[nodiscard]]
    virtual std::expected<GenericStream, sys::error_code> accept(Async) = 0;
};

class OuiServiceServer
{
public:
    OuiServiceServer(const asio::any_io_executor&);

    asio::any_io_executor get_executor() { return _ex; }

    void add(std::unique_ptr<OuiServiceImplementationServer> implementation);

    [[nodiscard]] sys::error_code start_listen(Async);
    void stop_listen();

    [[nodiscard]]
    std::expected<GenericStream, sys::error_code> accept(Async);
    void cancel_accept();

private:
    asio::any_io_executor _ex;

    std::vector<std::unique_ptr<OuiServiceImplementationServer>> _implementations;

    Cancel _stop_listen;
    std::list<GenericStream> _connection_queue;
    ConditionVariable _connection_available;
};

class OuiServiceImplementationClient
{
    public:
    virtual ~OuiServiceImplementationClient() {}

    [[nodiscard]]
    virtual sys::error_code  start(Async) = 0;

    virtual void stop() = 0;

    [[nodiscard]]
    virtual
    std::expected<GenericStream, sys::error_code> connect(Async) = 0;
};

/*
 * This temporary version supports only a single active implementation, and
 * therefore is just an empty shell. Later versions will support functionality
 * like trying multiple parallel implementations.
 */
class OuiServiceClient
{
    public:
    struct ConnectInfo {
        GenericStream connection;
        Endpoint remote_endpoint;
    };

    public:
    OuiServiceClient(const asio::any_io_executor&);

    void add(Endpoint, std::unique_ptr<OuiServiceImplementationClient>);

    [[nodiscard]]
    sys::error_code start(Async);

    void stop();

    [[nodiscard]]
    std::expected<ConnectInfo, sys::error_code> connect(Async);

    ~OuiServiceClient();

    private:
    std::optional<Endpoint> _endpoint;
    std::shared_ptr<OuiServiceImplementationClient> _implementation;
    bool _started;
    ConditionVariable _started_condition;
    Cancel _cancel;
};

} // ouinet namespace
