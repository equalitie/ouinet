#include "announcer.h"

using namespace ouinet;
using namespace ouinet::cache::bep5_http;

namespace bt = bittorrent;

struct Announcer::Entry {
    bt::Bep5Announcer bep5_announcer;
};

Announcer::Announcer(std::shared_ptr<bittorrent::MainlineDht> dht)
    : _dht(std::move(dht))
{}

void Announcer::add(Key key)
{
    auto infohash = util::sha1_digest(key);
    auto res = _entries.emplace(move(key), nullptr);
    
    if (res.second /* inserted? */) {
        res.first->second.reset(new Entry{{infohash, _dht}});
    }
}

Announcer::~Announcer() {}
