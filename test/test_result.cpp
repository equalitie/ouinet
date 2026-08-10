#define BOOST_TEST_MODULE cancel
#include <boost/test/unit_test.hpp>

#include <optional>
#include <expected>
#include <iostream>
#include "util/result.h"

using namespace ouinet;

struct NoDefaultConstruct {
    NoDefaultConstruct() = delete;
    NoDefaultConstruct(int v) : v(v) {}
    int v;
};

struct NoCopy {
    NoCopy() = default;
    NoCopy(NoCopy const&) = delete;
    NoCopy(NoCopy&&) = default;
    NoCopy& operator=(NoCopy&&) = default;
};

struct A {
    A() = default;
    A(const A&) = default;
    A(A&&) = default;
};

struct B {
    template <typename T>
    requires (std::constructible_from<A, T>)
    B(T&& t)
        : a(std::forward<T>(t))
    {}
    A a;
};

static_assert(!std::is_trivially_copy_constructible_v<NoCopy>);

BOOST_AUTO_TEST_CASE(sys_result) {
    {
        std::expected<void, sys::error_code> r;
        BOOST_CHECK(r.has_value());
    }
    {
        SysResult<void> r;
        BOOST_CHECK(r.has_value());
    }

    {
        std::expected<NoDefaultConstruct, sys::error_code> r = NoDefaultConstruct(0);
        BOOST_CHECK(r.has_value());
    }
    {
        SysResult<NoCopy> r = NoCopy();
        BOOST_CHECK(r.has_value());
    }

    {
        SysResult<NoCopy> r = NoCopy();
        BOOST_CHECK(r.has_value());
    }

    {
        std::optional<SysResult<NoCopy>> r;
        r = NoCopy();
        BOOST_CHECK(r);
        BOOST_CHECK(*r);
    }

    {
        std::expected<void, sys::error_code> exp;
        [[maybe_unused]]
        SysResult<void> result = exp;
    }

    {
        std::expected<NoCopy, sys::error_code> exp;
        [[maybe_unused]]
        SysResult<NoCopy> result = std::move(exp);
    }

    static_assert(std::is_constructible_v<std::expected<A, sys::error_code>, A>);
    static_assert(std::constructible_from<std::expected<A, sys::error_code>, std::expected<A, sys::error_code> const&>);

    {
        static_assert(std::is_convertible_v<A, B>);
        std::expected<A, sys::error_code> exp;
        [[maybe_unused]]
        std::expected<B, sys::error_code> result = std::move(exp);
    }
    {
        std::expected<A, sys::error_code> exp;
        [[maybe_unused]]
        SysResult<B> result = std::move(exp);
    }
}
