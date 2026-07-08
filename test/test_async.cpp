#define BOOST_TEST_MODULE async_completion_token
#include <boost/test/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <namespaces.h>
#include <util/async.h>
#include <util/wait_condition.h>

#include "util/async_test.h"
#include "util/unwrap.h"

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
    using ExecType = decltype(token.get_executor());

    return asio::async_initiate<Token, void(Args...)>(
        [ ...args = std::forward<Args>(args)  ] (auto handler) mutable {
            static_assert(std::is_same_v<
                asio::associated_executor_t<decltype(handler)>,
                ExecType
            >);

            asio::post(asio::get_associated_executor(handler),
                [ handler = std::move(handler),
                  ...args = std::forward<Args>(args) ] () mutable
                {
                    handler(std::forward<Args>(args)...);
                });
        },
        token
     );
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
    check_same<ActionResult<Async, sys::error_code>, std::expected<void, sys::error_code>>();
    check_same<ActionResult<Async, sys::error_code, int>, std::expected<int, sys::error_code>>();

    // Prevent warnings about test not running any checks
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(return_values) {
    async_test([] (Async yield) {
            unwrap(action(yield, sys::error_code{}));
        });

    async_test([] (Async yield) {
            auto r = unwrap(action(yield, sys::error_code{}, 1));
            BOOST_REQUIRE_EQUAL(r, 1);
        });

    async_test([] (Async yield) {
            unwrap(action(yield, sys::error_code{}, NoCopy()));
        });

    async_test([] (Async yield) {
            auto exp_ec = asio::error::operation_aborted;
            std::expected<void, sys::error_code> ex = action(yield, exp_ec);
            BOOST_REQUIRE(!ex.has_value());
            BOOST_REQUIRE_MESSAGE(ex.error() == exp_ec, ex.error().message());
        });

    async_test([] (Async yield) {
            auto exp_ec = asio::error::operation_aborted;
            std::expected<void, sys::error_code> ex = action(yield, exp_ec);
            BOOST_REQUIRE(!ex.has_value());
            BOOST_REQUIRE_MESSAGE(ex.error() == exp_ec, ex.error().message());
        });
}

BOOST_AUTO_TEST_CASE(asio_timer) {
    async_test([] (Async yield) {
            asio::steady_timer timer(yield.get_executor());
            timer.expires_after(10ms);
            unwrap(timer.async_wait(yield));
        });
}

BOOST_AUTO_TEST_CASE(cancel_timer) {
    async_test([] (Async yield) {
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
    async_test([] (Async yield) {
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
