#pragma once

#include "../../bittorrent/bep5_announcer.h"
#include "../../util/hash.h"
#include "../../logger.h"
#include <memory>

namespace ouinet { namespace cache { namespace bep5_http {

class Announcer {
private:
    struct Loop;

public:
    using Key = std::string;

    Announcer(std::shared_ptr<bittorrent::MainlineDht>, log_level_t);

    void add(Key key);
    void remove(const Key&);

    ~Announcer();

    void set_log_level(log_level_t);

private:
    std::unique_ptr<Loop> _loop;
};

}}} // namespaces
