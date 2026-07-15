#include "ouiservice.h"
#include "or_throw.h"
#include <boost/asio.hpp>
#include <expected>
#include "namespaces.h"

#include "util/wait_condition.h"
#include "util/str.h"
#include "util/async.h"
#include "task.h"
#include "async_sleep.h"

namespace ouinet {
using namespace std;

//--------------------------------------------------------------------
// OuiServiceServer
//--------------------------------------------------------------------

OuiServiceServer::OuiServiceServer(const AsioExecutor & ex):
    _ex(ex),
    _connection_available(ex)
{}

void OuiServiceServer::add(std::unique_ptr<OuiServiceImplementationServer> implementation)
{
    _implementations.push_back(std::move(implementation));
}

sys::error_code OuiServiceServer::start_listen(Async yield)
{
    using namespace std::chrono_literals;

    bool success = false;
    WaitCondition wc(_ex);
    auto lock = std::make_shared<WaitCondition::Lock>(wc.lock());

    for (auto& implementation : _implementations) {
        yield.spawn(_stop_listen, [
            this,
            &success,
            implementation = implementation.get(),
            lock
        ] (Async yield) mutable {
            sys::error_code ec;

            auto slot_connection = _stop_listen.connect([implementation] () {
                implementation->stop_listen();
            });

            ec = implementation->start_listen(yield);
            if (ec) return;

            success = true;
            lock->release();

            while (!_stop_listen) {
                auto connection = implementation->accept(yield);

                if (!connection.has_value()) {
                    async_sleep(1s, yield);
                    ec = sys::error_code();
                    continue;
                }

                _connection_queue.push_back(std::move(*connection));
                _connection_available.notify();
            }
        });
    }

    lock.reset();

    if (auto r = wc.wait(yield); !r) {
        return r.error();
    }

    return success ? sys::error_code() : asio::error::network_down;
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

std::expected<GenericStream, sys::error_code>
OuiServiceServer::accept(Async yield)
{
    if (_connection_queue.empty()) {
        if (auto r = _connection_available.wait(yield); !r) {
            return std::unexpected(r.error());
        }
    }

    if (_connection_queue.empty()) {
        return std::unexpected(asio::error::operation_aborted);
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

OuiServiceClient::OuiServiceClient(const AsioExecutor& ex):
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

sys::error_code OuiServiceClient::start(Async yield)
{
    assert(_implementation);

    _started = false;

    sys::error_code ec;

    decltype(_implementation) impl;

    do {
        impl = _implementation;
        ec = _implementation->start(yield);
    }
    while (_implementation && impl != _implementation);

    if (ec) return ec;

    _started = true;
    _started_condition.notify();

    return {};
}

void OuiServiceClient::stop()
{
    _cancel();
    if (!_implementation) return;

    _started = false;
    _implementation->stop();
    _started_condition.notify();
}

OuiServiceClient::~OuiServiceClient()
{
    stop();
}

std::expected<OuiServiceClient::ConnectInfo, sys::error_code>
OuiServiceClient::connect(Async yield)
{
    auto slot = _cancel.connect([&] { yield.cancel(); });

    namespace err = asio::error;

    if (!_implementation) {
        return std::unexpected(err::operation_not_supported);
    }

    if (!_started) {
        if (auto r = _started_condition.wait(yield); !r) {
            return std::unexpected(r.error());
        }
        if (!_started) {
            return std::unexpected(err::operation_aborted);
        }
    }

    std::expected<GenericStream, sys::error_code> con;
    decltype(_implementation) impl;

    do {
        impl = _implementation;
        con = _implementation->connect(yield);
    }
    while (_implementation && impl != _implementation);

    if (!con.has_value()) {
        return std::unexpected(con.error());
    }

    return ConnectInfo{std::move(*con), *_endpoint};
}

} // namespace
