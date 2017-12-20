#pragma once

#include <sys/resource.h>

namespace ouinet {

// Temporary, until this is merged https://github.com/ipfs/go-ipfs/pull/4288
// into IPFS.
inline
void increase_open_file_limit(rlim_t new_value)
{
    using namespace std;

    struct rlimit rl;

    int r = getrlimit(RLIMIT_NOFILE, &rl);

    if (r != 0) {
        cerr << "Failed to get the current RLIMIT_NOFILE value" << endl;
        return;
    }

    cout << "Default RLIMIT_NOFILE value is: " << rl.rlim_cur << endl;

    if (rl.rlim_cur >= new_value) {
        cout << "Leaving RLIMIT_NOFILE value unchanged." << endl;
        return;
    }

    rl.rlim_cur = new_value;

    r = setrlimit(RLIMIT_NOFILE, &rl);

    if (r != 0) {
        cerr << "Failed to set the RLIMIT_NOFILE value to " << rl.rlim_cur << endl;
        return;
    }

    r = getrlimit(RLIMIT_NOFILE, &rl);
    assert(r == 0);
    cout << "RLIMIT_NOFILE value changed to: " << rl.rlim_cur << endl;
}

} // ouinet namespace
