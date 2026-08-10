#pragma once

#include "namespaces.h"

#include <expected>
#include <boost/system/error_code.hpp>

namespace ouinet {

//
// Classes to mimic std::expected<T, E> with few niceties:
//
// * Is automatically [[nodiscard]]
// * Does runtime checks when asking for error or value
// * Has function `code()` which, unlike the `error()` function, returns
//   `sys::error_code()` (success) if the result contains a value.
//
// Feel free to add other functions which are in std::expected but not here as
// needed.
//

template<class V, class E> class [[nodiscard]] Result {
public:
    Result() = default;

    template<typename T>
    requires (
        !std::same_as<std::remove_cvref_t<V>, Result> &&
        std::constructible_from<std::expected<V, E>, T>
    )
    Result(T&& value) : _exp(std::forward<T>(value)) {}

    Result(Result const&) = default;
    Result(Result &&) = default;

    template<typename T>
    requires (
        !std::same_as<std::remove_cvref_t<V>, Result> &&
        std::constructible_from<std::expected<V, E>, V>
    )
    Result& operator=(T&& value) {
        _exp = std::forward<T>(value);
        return *this;
    }

    Result& operator=(Result const&) = default;
    Result& operator=(Result &&) = default;

    operator bool() const {
        return has_value();
    }

    bool has_value() const {
        return _exp.has_value();
    }

    auto operator*() -> std::add_lvalue_reference<V>
        requires (!std::is_same_v<V, void>)
    {
        if (!has_value()) std::terminate();
        return *_exp;
    }

    auto operator*() const -> std::add_lvalue_reference<V const>
        requires (!std::is_same_v<V, void>)
    {
        if (!has_value()) std::terminate();
        return *_exp;
    }

    V* operator->()
        requires (!std::is_same_v<V, void>)
    {
        if (!has_value()) std::terminate();
        return &*_exp;
    }

    V const* operator->() const
        requires (!std::is_same_v<V, void>)
    {
        if (!has_value()) std::terminate();
        return &*_exp;
    }

    const E& error() const {
        if (has_value()) std::terminate();
        return _exp.error();
    }

    const std::expected<V, E>& as_expected() const {
        return _exp;
    }

    sys::error_code code() const
        requires std::is_same_v<E, sys::error_code>
    {
        if (has_value()) sys::error_code();
        return error();
    }

protected:
    std::expected<V, E> _exp;
};

template<class V> using SysResult = Result<V, sys::error_code>;

} // namespace
