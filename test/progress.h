#pragma once

#include "../src/namespaces.h"

namespace ouinet {

struct Progress {
    Progress(const asio::executor& ex, std::string message)
        : _message(move(message))
    {
        using namespace std;

        asio::spawn(ex, [&] (asio::yield_context yield) {
            Cancel cancel(_cancel);
            const char p[] = {'|', '/', '-', '\\'};

            while (!cancel) {
                cerr << _message << "... " << p[_i++ % 4] << '\r';
                async_sleep(ex, chrono::milliseconds(200), cancel, yield);
            }
        });
    }

    ~Progress() {
        std::cerr << _message << " done. Took " << ((float) _i/5) << " seconds\r\n";
        _cancel();
    }

private:
    Cancel _cancel;
    std::string _message;
    unsigned _i = 0;
};

} // namespace
