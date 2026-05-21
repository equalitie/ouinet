#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>
#include "namespaces.h"

namespace ouinet {
    class GenericStream;
    class Async;
}

namespace ouinet::i2p_direct {

[[nodiscard]]
sys::error_code perform_handshake(GenericStream&, Async);

} // namespaces
