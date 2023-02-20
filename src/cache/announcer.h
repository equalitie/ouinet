#pragma once

#include "bittorrent/bep5_announcer.h"
#include "util/hash.h"
#include <memory>

namespace ouinet { namespace cache {

class Announcer {
private:
    struct Loop;

public:
    using Key = std::string;

    Announcer(std::shared_ptr<bittorrent::MainlineDht>, size_t);

    // Return true if the key was not being announced, false otherwise.
    bool add(Key key);
    // Return true if the key was being announced, false otherwise.
    bool remove(const Key&);

    ~Announcer();

private:
    std::unique_ptr<Loop> _loop;
};

}} // namespaces
