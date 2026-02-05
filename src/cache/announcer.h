#pragma once

#include "../bittorrent/bep5_announcer.h"
#include "../util/hash.h"
#include "../util/executor.h"
#include "namespaces.h"
#include <memory>

namespace ouinet { namespace cache {
using util::AsioExecutor;
}} // namespaces

#ifdef __EXPERIMENTAL__
namespace ouinet::ouiservice::i2poui { class Client; }
#endif

namespace ouinet { namespace cache {

// Base Announcer class with shared announcement loop logic
class Announcer {
public:
    struct Loop;
    using Key = std::string;

    Announcer(AsioExecutor ex, size_t simultaneous_announcements);

    // Return true if the key was not being announced, false otherwise.
    bool add(Key key);
    // Return true if the key was being announced, false otherwise.
    bool remove(const Key&);

    virtual ~Announcer();

protected:
    std::unique_ptr<Loop> _loop;
};

// BEP5 Announcer - announces to DHT
class Bep5Announcer : public Announcer {
public:
    Bep5Announcer(std::shared_ptr<bittorrent::DhtBase>, size_t simultaneous_announcements);
    ~Bep5Announcer();
};

#ifdef __EXPERIMENTAL__
// BEP3 Announcer - announces via HTTP to tracker over I2P
class Bep3Announcer : public Announcer {
public:
    Bep3Announcer( std::unique_ptr<ouiservice::i2poui::Client> i2p_client
                 , std::string serving_i2p_id
                 , size_t simultaneous_announcements);
    ~Bep3Announcer();
};
#endif // __EXPERIMENTAL__

}} // namespaces
