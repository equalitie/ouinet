#pragma once

#include "dht.h"

namespace ouinet { namespace bittorrent {

class MainlineDht;

namespace detail {
    struct Bep5AnnouncerImpl;
}

class Bep5PeriodicAnnouncer {
public:
    Bep5PeriodicAnnouncer() = default;

    Bep5PeriodicAnnouncer(NodeID infohash, std::weak_ptr<MainlineDht>);

    Bep5PeriodicAnnouncer(const Bep5PeriodicAnnouncer&)            = delete;
    Bep5PeriodicAnnouncer& operator=(const Bep5PeriodicAnnouncer&) = delete;

    Bep5PeriodicAnnouncer(Bep5PeriodicAnnouncer&&)            = default;
    Bep5PeriodicAnnouncer& operator=(Bep5PeriodicAnnouncer&&) = default;

    ~Bep5PeriodicAnnouncer();

private:
    std::shared_ptr<detail::Bep5AnnouncerImpl> _impl;
};

class Bep5ManualAnnouncer {
private:
    struct Impl;

public:
    Bep5ManualAnnouncer() = default;

    Bep5ManualAnnouncer(NodeID infohash, std::weak_ptr<MainlineDht>);

    Bep5ManualAnnouncer(const Bep5ManualAnnouncer&)            = delete;
    Bep5ManualAnnouncer& operator=(const Bep5ManualAnnouncer&) = delete;

    Bep5ManualAnnouncer(Bep5ManualAnnouncer&&)            = default;
    Bep5ManualAnnouncer& operator=(Bep5ManualAnnouncer&&) = default;

    ~Bep5ManualAnnouncer();

    void update();

private:
    std::shared_ptr<detail::Bep5AnnouncerImpl> _impl;
};

}} // namespaces
