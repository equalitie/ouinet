#include "ouiservice.h"
#include "or_throw.h"
#include <boost/asio.hpp>
#include "namespaces.h"
#include "blocker.h"

#include "util/condition_variable.h"
#include "util/success_condition.h"

namespace ouinet {

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

            implementation->start_listen(yield[ec]);
            if (ec) {
                return;
            }

            lock.release(true);

            bool running = true;
            auto slot_connection = _stop_listen.connect([implementation, &running] () {
                running = false;
                implementation->stop_listen();
            });

            while (true) {
                GenericConnection connection = implementation->accept(yield[ec]);
                /*
                 * TODO: Reconnect logic? There are errors other than operation_aborted.
                 */
                if (ec) {
                    break;
                }

                if (!running) {
                    connection.close();
                    break;
                }

                _connection_queue.push_back(std::move(connection));
                _connection_available.notify_one();
            }
        });
    }

    bool success = success_condition.wait_for_success(yield);

    if (!success) {
        or_throw(yield, boost::asio::error::network_down);
    }
}

void OuiServiceServer::stop_listen()
{
    _stop_listen();
    while (!_connection_queue.empty()) {
        _connection_queue.front().close();
        _connection_queue.pop_front();
    }
    _connection_available.notify_one();
}

GenericConnection OuiServiceServer::accept(asio::yield_context yield)
{
    if (_connection_queue.empty()) {
        _connection_available.wait(yield);
    }

    if (_connection_queue.empty()) {
        return or_throw<GenericConnection>(yield, boost::asio::error::operation_aborted);
    }

    GenericConnection connection = std::move(_connection_queue.front());
    _connection_queue.pop_front();
    return std::move(connection);
}

void OuiServiceServer::cancel_accept()
{
    _connection_available.notify_one();
}



OuiServiceClient::OuiServiceClient(asio::io_service& ios):
    _ios(ios)
{}

void OuiServiceClient::add(std::unique_ptr<OuiServiceImplementationClient> implementation)
{
    assert(_implementations.empty());
    _implementations.push_back(std::move(implementation));
}

GenericConnection OuiServiceClient::connect(asio::yield_context yield)
{
    assert(_implementations.size() == 1);
    return _implementations[0]->connect(yield);
}

void OuiServiceClient::cancel_connect()
{
    assert(_implementations.size() == 1);
    _implementations[0]->cancel_connect();
}

} // ouinet namespace
