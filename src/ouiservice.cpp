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

} // namespace
