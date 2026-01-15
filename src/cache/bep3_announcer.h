#pragma once

#include "util/hash.h"
#include <memory>

namespace ouinet { namespace cache {

class Bep3Announcer {
private:
    struct Loop;

public:
    using Key = std::string;

    Bep3Announcer(std::string TrackerId, size_t);

    // Return true if the key was not being announced, false otherwise.
    bool add(Key key);
    // Return true if the key was being announced, false otherwise.
    bool remove(const Key&);

    ~Bep3Announcer();

private:
    std::unique_ptr<Loop> _loop;
};

}} // namespaces
