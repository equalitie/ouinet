#include "client.h"
#include "logger.h"
#include "force_exit_on_signal.h"
#include <csignal>
#include <iostream>

using namespace ouinet;
using namespace std;

int main(int argc, const char* argv[])
{
  // When Stdout/stderr is  piped to a slow reader (e.g. the pytest harness).
  // A filled pipe + write() raises SIGPIPE, whose default action is to
  // terminate the process. We should ignore it so tests don't fail due
  // to too much logs.
    std::signal(SIGPIPE, SIG_IGN);

    ClientConfig cfg;

    try {
        cfg = ClientConfig(argc, argv);
    } catch(std::exception const& e) {
        LOG_ABORT(e.what());
        return 1;
    }

    if (cfg.is_help()) {
        cout << "Usage: client [OPTION...]" << endl;
        cout << cfg.description() << endl;
        return 0;
    }

    asio::io_context ctx;

    asio::signal_set signals(ctx, SIGINT, SIGTERM);

    Client client(ctx, move(cfg));

    unique_ptr<ForceExitOnSignal> force_exit;

    signals.async_wait([&client, &signals, &force_exit]
                       (const sys::error_code& ec, int signal_number) {
            LOG_INFO("GOT SIGNAL ", signal_number);
            HandlerTracker::stopped();
            client.stop();
            signals.clear();
            force_exit = make_unique<ForceExitOnSignal>();
        });

    try {
        client.start();
    } catch (std::exception& e) {
        LOG_ABORT(e.what());
        return 1;
    }

    ctx.run();

    LOG_INFO("Exiting gracefuly");

    return EXIT_SUCCESS;
}
