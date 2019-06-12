#pragma once

#include "dht.h"

namespace ouinet { namespace bittorrent {

class MainlineDht;

class Bep5Announcer {
private:
    struct Impl;

public:
    Bep5Announcer() = default;

    Bep5Announcer(NodeID infohash, std::weak_ptr<MainlineDht>);

    Bep5Announcer(const Bep5Announcer&)            = delete;
    Bep5Announcer& operator=(const Bep5Announcer&) = delete;

    Bep5Announcer(Bep5Announcer&&)            = default;
    Bep5Announcer& operator=(Bep5Announcer&&) = default;

    ~Bep5Announcer();

private:
    std::shared_ptr<Impl> _impl;
};

}} // namespaces
