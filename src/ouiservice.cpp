#include "ouiservice.h"
#include "or_throw.h"
#include <boost/asio.hpp>
#include "namespaces.h"

#include "util/condition_variable.h"
#include "util/success_condition.h"

using namespace std;
using namespace ouinet;

//--------------------------------------------------------------------
// OuiServiceServer
//--------------------------------------------------------------------

OuiServiceServer::OuiServiceServer(asio::io_service& ios):
    _ios(ios),
    _connection_available(ios)
{}

void OuiServiceServer::add(std::unique_ptr<OuiServiceImplementationServer> implementation)
{
    _implementations.push_back(std::move(implementation));
}

void OuiServiceServer::start_listen(asio::yield_context yield)
{
    SuccessCondition success_condition(_ios);

    for (auto& implementation : _implementations) {
        asio::spawn(_ios, [this, implementation = implementation.get(), lock = success_condition.lock()] (asio::yield_context yield) mutable {
            sys::error_code ec;

            auto slot_connection = _stop_listen.connect([implementation] () {
                implementation->stop_listen();
            });

            implementation->start_listen(yield[ec]);
            if (ec) {
                return;
            }

            lock.release(true);

            while (true) {
                GenericConnection connection = implementation->accept(yield[ec]);
                /*
                 * TODO: Reconnect logic? There are errors other than operation_aborted.
                 */
                if (ec) {
                    break;
                }

                if (_stop_listen.call_count()) {
                    connection.close();
                    break;
                }

                _connection_queue.push_back(std::move(connection));
                _connection_available.notify();
            }
        });
    }

    bool success = success_condition.wait_for_success(yield);

    if (!success) {
        or_throw(yield, asio::error::network_down);
    }
}

void OuiServiceServer::stop_listen()
{
    _stop_listen();
    while (!_connection_queue.empty()) {
        _connection_queue.front().close();
        _connection_queue.pop_front();
    }
    _connection_available.notify();
}

GenericConnection OuiServiceServer::accept(asio::yield_context yield)
{
    if (_connection_queue.empty()) {
        _connection_available.wait(yield);
    }

    if (_connection_queue.empty()) {
        return or_throw<GenericConnection>(yield, asio::error::operation_aborted);
    }

    GenericConnection connection = std::move(_connection_queue.front());
    _connection_queue.pop_front();
    return std::move(connection);
}

void OuiServiceServer::cancel_accept()
{
    _connection_available.notify();
}

//--------------------------------------------------------------------
// OuiServiceClient
//--------------------------------------------------------------------

OuiServiceClient::OuiServiceClient(asio::io_service& ios):
    _ios(ios),
    _started(false),
    _started_condition(ios)
{}

void OuiServiceClient::add(std::unique_ptr<OuiServiceImplementationClient> implementation)
{
    // TODO: Currently _adding_ with actually _swap_ the previous
    // implementation for the new one.

    if (_implementation) {
        _implementation->stop();
    }

    _implementation = std::move(implementation);
}

void OuiServiceClient::start(asio::yield_context yield)
{
    assert(_implementation);

    _started = false;

    sys::error_code ec;

    decltype(_implementation) impl;

    do {
        impl = _implementation;
        _implementation->start(yield[ec]);
    }
    while (_implementation && impl != _implementation);

    if (ec) return or_throw(yield, ec);

    _started = true;
    _started_condition.notify();
}

void OuiServiceClient::stop()
{
    assert(_implementation);

    _started = false;
    _implementation->stop();
    _started_condition.notify();
}

OuiServiceImplementationClient::ConnectInfo
OuiServiceClient::connect(asio::yield_context yield, Signal<void()>& cancel)
{
    using ConnectInfo = OuiServiceImplementationClient::ConnectInfo;

    namespace err = asio::error;

    if (!_implementation) {
        return or_throw<ConnectInfo>(yield, err::operation_not_supported);
    }

    if (!_started) {
        _started_condition.wait(yield);
        if (!_started) {
            return or_throw<ConnectInfo>(yield, err::operation_aborted);
        }
    }

    ConnectInfo retval;
    sys::error_code ec;
    decltype(_implementation) impl;

    do {
        ec = sys::error_code();
        impl = _implementation;
        retval = _implementation->connect(yield[ec], cancel);
    }
    while (_implementation && impl != _implementation);

    return or_throw(yield, ec, move(retval));
}
