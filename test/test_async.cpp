#define BOOST_TEST_MODULE async_completion_token
#include <boost/test/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <namespaces.h>
#include <util/async.h>
#include <util/wait_condition.h>

using namespace ouinet;
using namespace std::chrono_literals;

// Define async action for testing basic functionality before testing with asio
// IO objects. The `Args` are arguments that would normally be passed to a
// completion handler, such as `void()`, `void(sys::error_code)`,
// `void(sys::error_code, size_t)`,...
template<
    asio::completion_token_for<void(sys::error_code)> Token,
    class... Args 
>
auto action(Token token, Args&&... args)
{
    auto exec = token.get_executor();
    return asio::async_initiate<Token, void(Args...)>(
        [ exec, ...args = std::forward<Args>(args)  ] (auto handler) mutable {
            asio::post(exec,
                [ handler = std::move(handler),
                  ...args = std::forward<Args>(args) ] () mutable
                {
                    handler(std::forward<Args>(args)...);
                });
        },
        token
     );
}

void test(auto work) {
    asio::io_context ctx;

    asio::spawn(ctx, [work = std::move(work)] (asio::yield_context yield) mutable {
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
        });

    ctx.run();
}

// Get the return type of calling the above `action` when invoked with given arguments.
template<typename... Ts> using ActionResult = decltype(action(std::declval<Ts>()...));

// Check that the types T1 and T2 are the same. Note that we could have used
// `static_assert(std::is_same_v<T1, T2>)`, but that would only show whether
// the assertion passed or not. The error message when using `check_same` also
// shows the types that were passed to it.
template<typename T1, typename T2> class check_same;
template<typename T>               class check_same<T, T> {};

struct NoCopy {
    NoCopy() = default;
    NoCopy(NoCopy&&) = default;
    NoCopy(NoCopy const&) = delete;
};

BOOST_AUTO_TEST_CASE(static_return_types) {
    // For comparison
    static_assert(asio::completion_token_for<asio::yield_context, void(sys::error_code)>);
    static_assert(asio::completion_token_for<asio::yield_context&, void(sys::error_code)>);
    static_assert(asio::completion_token_for<asio::yield_context&&, void(sys::error_code)>);

    static_assert(asio::completion_token_for<Async, void(sys::error_code)>);
    static_assert(asio::completion_token_for<Async&, void(sys::error_code)>);
    static_assert(asio::completion_token_for<Async&&, void(sys::error_code)>);

    // Static test expected return types from calling `action` with different arguments
    check_same<ActionResult<Async>, void>();
    check_same<ActionResult<Async, sys::error_code>, sys::error_code>();
    check_same<ActionResult<Async, sys::error_code, int>, std::expected<int, sys::error_code>>();

    // Prevent warnings about test not running any checks
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(return_values) {
    test([] (Async yield) {
            sys::error_code ec = action(yield, sys::error_code{});
            BOOST_REQUIRE_MESSAGE(!ec, ec.message());
        });

    test([] (Async yield) {
            auto r = action(yield, sys::error_code{}, 1);
            BOOST_REQUIRE_MESSAGE(r.has_value(), r.error());
            BOOST_REQUIRE_EQUAL(*r, 1);
        });

    test([] (Async yield) {
            auto r = action(yield, sys::error_code{}, NoCopy());
            BOOST_REQUIRE_MESSAGE(r.has_value(), r.error());
        });

    test([] (Async yield) {
            auto exp_ec = asio::error::operation_aborted;
            sys::error_code ec = action(yield, exp_ec);
            BOOST_REQUIRE_MESSAGE(ec = exp_ec, ec.message());
        });

    test([] (Async yield) {
            auto exp_ec = asio::error::operation_aborted;
            sys::error_code ec = action(yield, exp_ec);
            BOOST_REQUIRE_MESSAGE(ec = exp_ec, ec.message());
        });
}

BOOST_AUTO_TEST_CASE(asio_timer) {
    test([] (Async yield) {
            asio::steady_timer timer(yield.get_executor());
            timer.expires_after(10ms);
            sys::error_code ec = timer.async_wait(yield);
            BOOST_REQUIRE_MESSAGE(!ec, ec.message());
        });
}

BOOST_AUTO_TEST_CASE(cancel_timer) {
    test([] (Async yield) {
            auto exec = yield.get_executor();

            WaitCondition wc(exec);
            asio::steady_timer timer(exec);

            // Make a copy so that calling `cancel` won't cancel the main `yield`.
            auto y = yield;
            auto slot = y.cancel_slot([&] { timer.cancel(); });

            bool started_waiting = false;
            y.spawn([&, lock = wc.lock()] (auto yield) {
                    timer.expires_after(asio::steady_timer::duration::max());
                    started_waiting = true;
                    timer.async_wait(yield);
                    BOOST_FAIL("Unreachable");
                });

            BOOST_CHECK(started_waiting);
            y.cancel();

            wc.wait(yield);
        });
}

void sleep_forever(Async yield) {
    asio::steady_timer timer(yield.get_executor());
    auto slot = yield.cancel_slot([&] { timer.cancel(); });
    while (true) {
        timer.expires_after(1s);
        timer.async_wait(yield);
    }
}

BOOST_AUTO_TEST_CASE(cancel_yield) {
    test([] (Async yield) {
            auto exec = yield.get_executor();

            WaitCondition wc(exec);

            // Make a copy so that calling `cancel` won't cancel the main `yield`.
            auto y = yield;

            y.spawn([lock = wc.lock()] (auto yield) {
                sleep_forever(yield);
                BOOST_FAIL("Unreachable");
            });

            y.cancel();
            wc.wait(yield);
            BOOST_CHECK(true);
        });
}
