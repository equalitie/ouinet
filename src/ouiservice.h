#pragma once

#include <list>
#include <memory>
#include <vector>

#include <boost/asio/spawn.hpp>

#include "generic_stream.h"
#include "endpoint.h"
#include "util/condition_variable.h"
#include "util/cancel.h"
#include "api.h"

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

class OUINET_COMMON_API OuiServiceServer
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

class OuiServiceClient
{
    public:
    virtual ~OuiServiceClient() {}

    [[nodiscard]]
    virtual sys::error_code  start(Async) = 0;

    [[nodiscard]]
    virtual
    std::expected<GenericStream, sys::error_code> connect(Async) = 0;
};

} // ouinet namespace
