#pragma once

#include <boost/asio/spawn.hpp>
#include "util/signal.h"

namespace ouinet {
    class GenericStream;
    class Async;
}

namespace ouinet::i2p_direct {

[[nodiscard]]
sys::error_code perform_handshake(GenericStream&, Async);

} // namespaces
