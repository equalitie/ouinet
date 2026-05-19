#pragma once

#include "ouiservice/i2p/session.h"
#include "util/cancel.h"
#include "util/promise.h"
#include "declspec.h"

namespace ouinet {

namespace util { class LogPath; }

using CreateI2pSessionPromise = Promise<
            std::expected<
                std::shared_ptr<I2pSession>,
                I2pSession::Error::Create
            >
        >;

CreateI2pSessionPromise::Future
OUINET_DECL create_i2p_session(Cancel, util::LogPath, asio::any_io_executor);

} // namespace
