#pragma once

#include <boost/variant.hpp>

namespace ouinet { namespace util {

namespace __variant_detail {
    // With C++17 we'd only need this:
    //template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

    template<class T, class... Ts> struct overloaded : T, overloaded<Ts...> {
        using T::operator();
        using overloaded<Ts...>::operator();

        overloaded(T t, Ts... ts)
            : T(std::move(t))
            , overloaded<Ts...>(std::move(ts)...)
        {}
    };

    template<class T> struct overloaded<T> : T {
        using T::operator();

        overloaded(T t)
            : T(std::move(t))
        {}
    };
}

/* 
 * Inspired by the `overloaded` thing in 
 * https://en.cppreference.com/w/cpp/utility/variant/visit
 *
 * The function `apply` is meant to make work with boost::variant easier.
 *
 * Example:
 *
 *   using Number = variant<int, float>;
 *   Number number;
 *
 *   Number num_plu_one = apply(number
 *                             , [] (int i)   { return Number(i + 1); }
 *                             , [] (float f) { return Number(f + 1); });
 */

template<class Variant, class... Fs>
auto apply(Variant&& v, Fs&&... fs) {
    return boost::apply_visitor(
            __variant_detail::overloaded<Fs...>{std::forward<Fs>(fs)...},
            std::forward<Variant>(v));
}

}} // namespaces
