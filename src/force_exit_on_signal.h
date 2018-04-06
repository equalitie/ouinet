#pragma once

#include <thread>

namespace ouinet {

// A RAII structure to install a signal handler to forcefuly close the
// application. Note that we can't simply install such signal handler on the
// main io_service because that would prevent that io_service from finishing
// it's `run` function. Instead, we create a new io_service and run it in a
// separate thread so that it doesn't block the rest of the app.
struct ForceExitOnSignal {
    ForceExitOnSignal()
    {
        _thread = std::thread([this] {
            asio::signal_set signals(_ios, SIGINT, SIGTERM);
            signals.async_wait([] (const sys::error_code&, int) { exit(1); });
            _ios.run();
        });
    }

    ~ForceExitOnSignal() {
        _ios.stop();
        _thread.join();
    }

    asio::io_service _ios;
    std::thread _thread;
};

} // ouinet namespace
