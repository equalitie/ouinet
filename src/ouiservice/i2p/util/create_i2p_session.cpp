#include "create_i2p_session.h"
#include "task.h"

namespace ouinet {

CreateI2pSessionPromise::Future
create_i2p_session(Cancel cancel, util::LogPath log_path, asio::any_io_executor exec) {
    auto promise = CreateI2pSessionPromise(exec);
    
    task::spawn_detached(exec, [
        promise,
        cancel,
        log_path = std::move(log_path)
    ]
    (asio::yield_context yield) mutable {
        auto i2p_session = I2pSession::create(Async(yield, cancel, log_path.tag("I2pSession::create")));

        if (i2p_session.has_value()) {
            auto ptr = std::make_shared<I2pSession>(std::move(*i2p_session));
            promise.set_value(ptr);
        } else {
            promise.set_value(std::unexpected(i2p_session.error()));
        }
    });
    
    return promise.get_future();
}


} // namespace
