#pragma once

#include "dht.h"

namespace ouinet { namespace bittorrent {

class MainlineDht;

class Bep5PeriodicAnnouncer {
private:
    struct Impl;

public:
    Bep5PeriodicAnnouncer() = default;

    Bep5PeriodicAnnouncer(NodeID infohash, std::weak_ptr<MainlineDht>);

    Bep5PeriodicAnnouncer(const Bep5PeriodicAnnouncer&)            = delete;
    Bep5PeriodicAnnouncer& operator=(const Bep5PeriodicAnnouncer&) = delete;

    Bep5PeriodicAnnouncer(Bep5PeriodicAnnouncer&&)            = default;
    Bep5PeriodicAnnouncer& operator=(Bep5PeriodicAnnouncer&&) = default;

    ~Bep5PeriodicAnnouncer();

private:
    std::shared_ptr<Impl> _impl;
};

}} // namespaces
