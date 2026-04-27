#pragma once

#include <boost/asio/spawn.hpp>
#include "util/signal.h"

namespace ouinet {
    class GenericStream;
}

namespace ouinet::i2p_direct {

void perform_handshake(GenericStream& conn, Cancel& cancel, asio::yield_context yield);

} // namespaces
