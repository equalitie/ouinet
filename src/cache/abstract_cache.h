#pragma once

#include "cache_entry.h"
#include "../util/yield.h"

namespace ouinet {

class AbstractCache {
public:
    using Response = CacheEntry::Response;

    virtual
    CacheEntry load(const std::string& key, Cancel, Yield) = 0;

    virtual
    void store(const std::string& key, Response&, Cancel, asio::yield_context) = 0;
};

} // namespace
