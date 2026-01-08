#include "injector.h"
#include <boost/asio/signal_set.hpp>
#include "force_exit_on_signal.h"

using namespace std;
using namespace ouinet;

int main(int argc, const char* argv[])
{
    util::crypto_init();

    InjectorConfig config;

    try {
        config = InjectorConfig(argc, argv);
    }
    catch(const exception& e) {
        LOG_ABORT(e.what());
        return 1;
    }

    if (config.is_help()) {
        cout << "Usage: injector [OPTION...]" << endl;
        cout << config.options_description() << endl;
        return EXIT_SUCCESS;
    }

    asio::io_context ctx;

    Injector injector(std::move(config), ctx);

    asio::signal_set signals(ctx.get_executor(), SIGINT, SIGTERM);

    unique_ptr<ForceExitOnSignal> force_exit;

    signals.async_wait([&injector, &signals, &force_exit]
                       (const sys::error_code& ec, int signal_number) {
            injector.stop();
            signals.clear();
            force_exit = make_unique<ForceExitOnSignal>();
        });

    ctx.run();

    return EXIT_SUCCESS;
}
