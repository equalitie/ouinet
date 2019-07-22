#pragma once

#include "../../bittorrent/bep5_announcer.h"
#include "../../util/hash.h"
#include <map>

namespace ouinet { namespace cache { namespace bep5_http {

class Announcer {
private:
    struct Entry;

public:
    using Key = std::string;

    Announcer(std::shared_ptr<bittorrent::MainlineDht> dht);

    void add(Key key);

    ~Announcer();

private:
    std::shared_ptr<bittorrent::MainlineDht> _dht;
    std::map<Key, std::unique_ptr<Entry>> _entries;
};

}}} // namespaces
