#pragma once

#include <string>

namespace ouinet {

struct SessionId {
    std::string value;
    static SessionId random();
};

} // namespace
