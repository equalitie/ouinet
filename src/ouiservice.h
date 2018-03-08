#pragma once

#include <list>
#include <memory>
#include <vector>

#include <boost/asio/spawn.hpp>

#include "generic_connection.h"
#include "util/condition_variable.h"
#include "util/signal.h"

namespace ouinet {

class OuiServiceImplementationServer
{
    public:
    virtual ~OuiServiceImplementationServer() {}

    virtual void start_listen(boost::asio::yield_context yield) = 0;
    virtual void stop_listen() = 0;

    virtual GenericConnection accept(boost::asio::yield_context yield) = 0;
};

class OuiServiceServer
{
    public:
    OuiServiceServer(boost::asio::io_service& ios);

    boost::asio::io_service& get_io_service() { return _ios; }

    void add(std::unique_ptr<OuiServiceImplementationServer> implementation);

    void start_listen(boost::asio::yield_context yield);
    void stop_listen();

    GenericConnection accept(boost::asio::yield_context yield);
    void cancel_accept();

    private:
    boost::asio::io_service& _ios;

    std::vector<std::unique_ptr<OuiServiceImplementationServer>> _implementations;

    Signal<void()> _stop_listen;
    std::list<GenericConnection> _connection_queue;
    ConditionVariable _connection_available;
};

class OuiServiceImplementationClient
{
    public:
    virtual ~OuiServiceImplementationClient() {}

    virtual GenericConnection connect(boost::asio::yield_context yield) = 0;
    virtual void cancel_connect() = 0;
};

/*
 * This temporary version supports only a single active implementation, and
 * therefore is just an empty shell.Later versions will support functionality
 * like trying multiple parallel implementations.
 */
class OuiServiceClient
{
    public:
    OuiServiceClient(boost::asio::io_service& ios);

    void add(std::unique_ptr<OuiServiceImplementationClient> implementation);

    GenericConnection connect(boost::asio::yield_context yield);
    void cancel_connect();

    private:
    boost::asio::io_service& _ios;
    std::vector<std::unique_ptr<OuiServiceImplementationClient>> _implementations;
};

} // ouinet namespace
