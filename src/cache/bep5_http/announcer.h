#pragma once

#include "../../bittorrent/bep5_announcer.h"
#include "../../util/hash.h"
#include <memory>

namespace ouinet { namespace cache { namespace bep5_http {

class Announcer {
private:
    struct Loop;

public:
    using Key = std::string;

    Announcer(std::shared_ptr<bittorrent::MainlineDht>);

    void add(Key key);

    ~Announcer();

private:
    std::unique_ptr<Loop> _loop;
};

}}} // namespaces
