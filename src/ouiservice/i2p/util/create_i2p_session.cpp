#include "create_i2p_session.h"
#include "ouiservice/i2p/address.h"
#include "task.h"
#include "logger.h"

namespace ouinet {

CreateI2pSessionPromise::Future
create_i2p_session(Cancel cancel, util::LogPath log_path, asio::any_io_executor exec) {
    auto promise = CreateI2pSessionPromise(exec);
    
    task::spawn_detached(exec, [
        promise,
        cancel,
        log_path = log_path.tag("create_i2p_session")
    ]
    (asio::yield_context yield) mutable {
        auto i2p_session = I2pSession::create(Async(yield, cancel, log_path));

        if (i2p_session.has_value()) {
            // Used by python test
            {
                auto b32 = i2p_session->local_addr().to_b32();
                LOG_DEBUG(log_path, " I2P Session created, local_addr: ", b32);
            }
            auto ptr = std::make_shared<I2pSession>(std::move(*i2p_session));
            promise.set_value(ptr);
        } else {
            promise.set_value(std::unexpected(i2p_session.error()));
        }
    });
    
    return promise.get_future();
}


} // namespace
