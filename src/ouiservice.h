#pragma once

#include <list>
#include <memory>
#include <vector>

#include <boost/asio/spawn.hpp>

#include "generic_stream.h"
#include "endpoint.h"
#include "util/condition_variable.h"
#include "util/signal.h"

namespace ouinet {

class OuiServiceImplementationServer
{
    public:
    virtual ~OuiServiceImplementationServer() {}

    virtual void start_listen(asio::yield_context yield) = 0;
    virtual void stop_listen() = 0;

    virtual GenericStream accept(asio::yield_context yield) = 0;
};

class OuiServiceServer
{
    public:
    OuiServiceServer(const asio::executor&);

    asio::executor get_executor() { return _ex; }

    void add(std::unique_ptr<OuiServiceImplementationServer> implementation);

    /*
     * TODO: Should this have start() and stop() in addition to *_listen()?
     */

    void start_listen(asio::yield_context yield);
    void stop_listen();

    GenericStream accept(asio::yield_context yield);
    void cancel_accept();

    private:
    asio::executor _ex;

    std::vector<std::unique_ptr<OuiServiceImplementationServer>> _implementations;

    Signal<void()> _stop_listen;
    std::list<GenericStream> _connection_queue;
    ConditionVariable _connection_available;
};

class OuiServiceImplementationClient
{
    public:
    virtual ~OuiServiceImplementationClient() {}

    virtual void start(asio::yield_context yield) = 0;
    virtual void stop() = 0;

    virtual GenericStream connect(asio::yield_context yield, Signal<void()>& cancel) = 0;
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
    OuiServiceClient(const asio::executor&);

    void add(Endpoint, std::unique_ptr<OuiServiceImplementationClient>);

    void start(asio::yield_context yield);
    void stop();

    ConnectInfo
    connect(asio::yield_context yield, Signal<void()>& cancel);

    private:
    Endpoint _endpoint;
    std::shared_ptr<OuiServiceImplementationClient> _implementation;
    bool _started;
    ConditionVariable _started_condition;
};

} // ouinet namespace
