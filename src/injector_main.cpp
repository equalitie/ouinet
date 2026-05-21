#include "injector.h"
#include <boost/asio/signal_set.hpp>
#include "force_exit_on_signal.h"
#include <csignal>

using namespace std;
using namespace ouinet;

int main(int argc, const char* argv[])
{
    // When Stdout/stderr is piped to a slow reader (e.g. the pytest harness).
    // A filled pipe + write() raises SIGPIPE, whose default action is to
    // terminate the process. We should ignore it so tests don't fail due
    // to too much logs.
    std::signal(SIGPIPE, SIG_IGN);

    InjectorConfig config;

    try {
        config = InjectorConfig(argc, argv);
    }
    catch(const std::exception& e) {
        LOG_ABORT(e.what());
        return 1;
    }

    if (config.is_help()) {
        std::cout << "Usage: injector [OPTION...]" << std::endl;
        std::cout << config.options_description() << std::endl;
        return EXIT_SUCCESS;
    }

    asio::io_context ctx;

    Injector injector(std::move(config), ctx);

    asio::signal_set signals(ctx.get_executor(), SIGINT, SIGTERM);

    std::unique_ptr<ForceExitOnSignal> force_exit;

    signals.async_wait([&injector, &signals, &force_exit]
                       (const sys::error_code& ec, int signal_number) {
            injector.stop();
            signals.clear();
            force_exit = std::make_unique<ForceExitOnSignal>();
        });

    ctx.run();

    return EXIT_SUCCESS;
}
