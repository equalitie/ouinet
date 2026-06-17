#pragma once

#include <type_traits>
#include <boost/asio.hpp>
#include <util/async.h>

namespace ouinet {

template<typename F>
requires std::invocable<F, ouinet::Async>
void async_test(asio::io_context& ctx, F work) {
    boost::asio::spawn(
        ctx,
        [work = std::forward<F>(work)] (asio::yield_context yield) mutable {
            // Wrap `asio::yield_context` in `Async` and pass `util::LogPath`
            // to it for convenient logging.
            work(
                Async(
                    yield,
                    util::LogPath(
                        boost::unit_test::framework::current_test_case().p_name
                    )
                )
            );
        },
        [] (std::exception_ptr ep) {
            // We don't expect exceptions, results from async actions using
            // `Async` are all of type `std::expected` and we explicitly check
            // their `.has_value()`.
            try {
                if (ep) std::rethrow_exception(ep);
            }
            catch (std::exception const& e) {
                BOOST_ERROR("Exception: " << e.what());
            }
            catch (...) {
                BOOST_ERROR("Unknown exception");
            }
        }
    );
}

// Run the given `void(Async)` function and block until it completes.
template<typename F>
requires std::invocable<F, ouinet::Async>
void async_test(F work) {
    boost::asio::io_context ctx;
    async_test(ctx, std::forward<F>(work));
    ctx.run();
}

} // namespace ouinet
