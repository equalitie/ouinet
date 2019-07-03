#pragma once

#include "cache_entry.h"
#include "../util/yield.h"
#include "../generic_stream.h"

namespace ouinet {

class AbstractCache {
public:
    virtual
    void load( const std::string& key
             , GenericStream& sink
             , Cancel
             , Yield) = 0;

    virtual
    void store( const std::string& key
              , const http::response_header<>&
              , GenericStream& response_body
              , Cancel
              , asio::yield_context) = 0;
};

} // namespace
