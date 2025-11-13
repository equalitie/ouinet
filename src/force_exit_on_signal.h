#pragma once

#include <thread>
#include <boost/asio/signal_set.hpp>
#include "namespaces.h"

namespace ouinet {

// A RAII structure to install a signal handler to forcefuly close the
// application. Note that we can't simply install such signal handler on the
// main io_service because that would prevent that io_service from finishing
// it's `run` function. Instead, we create a new io_service and run it in a
// separate thread so that it doesn't block the rest of the app.
class ForceExitOnSignal {
public:
    ForceExitOnSignal()
    {
        _thread = std::thread([this] {
            asio::signal_set signals(_ctx, SIGINT, SIGTERM);
            signals.async_wait([] (const sys::error_code&, int) { exit(1); });
            _ctx.run();
        });
    }

    ~ForceExitOnSignal()
    {
        _ctx.stop();
        _thread.join();
    }

private:
    asio::io_context _ctx;
    std::thread _thread;
};

} // ouinet namespace
