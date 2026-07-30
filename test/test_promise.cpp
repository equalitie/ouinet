#define BOOST_TEST_MODULE promise
#include <boost/test/unit_test.hpp>

#include "util/promise.h"
#include "namespaces.h"

#include <boost/asio/spawn.hpp>

using namespace ouinet;

template<class F> void test_spawn(asio::any_io_executor exec, F f) {
    asio::spawn(exec,
        [ f = std::move(f) ] (asio::yield_context yield) mutable {
            f(Async(yield));
        },
        [] (std::exception_ptr e) {
            try {
                if (e) std::rethrow_exception(e);
            }
            catch (std::exception const& e) {
                BOOST_FAIL(e.what());
                throw;
            }
        }
    );
}

template<class F> void test_run(F f) {
    asio::io_context ctx;
    test_spawn(ctx.get_executor(), std::move(f));
    ctx.run();
}

BOOST_AUTO_TEST_CASE(sanity) {
    test_run([] (auto yield) {
        Promise<bool> p(yield.get_executor());
        p.set_value(true);
        auto f = p.get_future();
        auto value = f.wait(yield);
        BOOST_REQUIRE(value.has_value());
        BOOST_REQUIRE_EQUAL(*value, true);
    });

    test_run([] (auto yield) {
        Promise<bool> p(yield.get_executor());
        auto f = p.get_future();

        test_spawn(yield.get_executor(), [p = std::move(p)] (auto yield) mutable {
            asio::defer(yield);
            p.set_value(true);
        });

        auto value = f.wait(yield);
        BOOST_REQUIRE(value.has_value());
        BOOST_REQUIRE_EQUAL(*value, true);
    });

    test_run([] (auto yield) {
        struct Foo {
            std::shared_ptr<bool> destroyed;

            Foo(std::shared_ptr<bool> destroyed) : destroyed(destroyed) {}
            Foo(Foo&&) = default;
            ~Foo() { if (destroyed) *destroyed = true; }
        };

        auto destroyed = std::make_shared<bool>(false);

        {
            std::optional<typename Promise<Foo>::Future> f;

            {
                Promise<Foo> p(yield.get_executor());
                p.set_value(Foo(destroyed));
                f = p.get_future();
            }

            BOOST_REQUIRE_EQUAL(*destroyed, false);
        }

        BOOST_REQUIRE_EQUAL(*destroyed, true);
    });
}

BOOST_AUTO_TEST_CASE(ready) {
    test_run([] (auto yield) {
        int v = 42;
        using P = Promise<decltype(v)>;
        auto f = P::Future::make_ready(v);

        auto value = f.wait(yield);
        BOOST_REQUIRE(value.has_value());
        BOOST_REQUIRE_EQUAL(*value, v);
    });

}
