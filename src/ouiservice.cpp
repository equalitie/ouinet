#include "ouiservice.h"
#include "or_throw.h"
#include <boost/asio.hpp>
#include "namespaces.h"

#include "util/condition_variable.h"
#include "util/success_condition.h"
#include "util/str.h"
#include "util/handler_tracker.h"
#include "async_sleep.h"

using namespace std;
using namespace ouinet;

//--------------------------------------------------------------------
// OuiServiceServer
//--------------------------------------------------------------------

OuiServiceServer::OuiServiceServer(const asio::executor& ex):
    _ex(ex),
    _connection_available(ex)
{}

void OuiServiceServer::add(std::unique_ptr<OuiServiceImplementationServer> implementation)
{
    _implementations.push_back(std::move(implementation));
}

void OuiServiceServer::start_listen(asio::yield_context yield)
{
    using namespace std::chrono_literals;

    SuccessCondition success_condition(_ex);

    for (auto& implementation : _implementations) {
        TRACK_SPAWN(_ex, ([
            this,
            implementation = implementation.get(),
            lock = success_condition.lock()
        ] (asio::yield_context yield) mutable {
            sys::error_code ec;

            auto slot_connection = _stop_listen.connect([implementation] () {
                implementation->stop_listen();
            });

            implementation->start_listen(yield[ec]);

            if (ec) return;

            lock.release(true);

            while (!_stop_listen) {
                GenericStream connection = implementation->accept(yield[ec]);

                if (ec == asio::error::operation_aborted) {
                    break;
                }

                if (ec) {
                    // Retry after a short while to avoid CPU hogging
                    async_sleep(_ex, 1s, _stop_listen, yield);
                    ec = sys::error_code();
                    continue;
                }

                if (_stop_listen) {
                    connection.close();
                    break;
                }

                _connection_queue.push_back(std::move(connection));
                _connection_available.notify();
            }
        }));
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

GenericStream OuiServiceServer::accept(asio::yield_context yield)
{
    if (_connection_queue.empty()) {
        _connection_available.wait(yield);
    }

    if (_connection_queue.empty()) {
        return or_throw<GenericStream>(yield, asio::error::operation_aborted);
    }

    GenericStream connection = std::move(_connection_queue.front());
    _connection_queue.pop_front();
    return connection;
}

void OuiServiceServer::cancel_accept()
{
    _connection_available.notify();
}

//--------------------------------------------------------------------
// OuiServiceClient
//--------------------------------------------------------------------

OuiServiceClient::OuiServiceClient(const asio::executor& ex):
    _started(false),
    _started_condition(ex)
{}

void OuiServiceClient::add( Endpoint endpoint
                          , std::unique_ptr<OuiServiceImplementationClient> implementation)
{
    // TODO: Currently _adding_ with actually _swap_ the previous
    // implementation for the new one.

    if (_implementation) {
        _implementation->stop();
    }

    _endpoint = std::move(endpoint);
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
    if (!_implementation) return;

    _started = false;
    _implementation->stop();
    _started_condition.notify();
}

OuiServiceClient::ConnectInfo
OuiServiceClient::connect(asio::yield_context yield, Signal<void()>& cancel)
{
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

    GenericStream con;
    sys::error_code ec;
    decltype(_implementation) impl;

    do {
        ec = sys::error_code();
        impl = _implementation;
        con = _implementation->connect(yield[ec], cancel);
    }
    while (_implementation && impl != _implementation);

    return or_throw<ConnectInfo>(yield, ec, {move(con), _endpoint});
}
