#pragma once

// Utilities to work with `std::expected`.

namespace ouinet {

// Determines whether `T` is `std::expected<A, B>` for any `A` and `B`.
template<typename T>
struct is_expected : std::false_type {};

template<typename T, typename E>
struct is_expected<std::expected<T, E>> : std::true_type {};

template<typename T>
constexpr bool is_expected_v = is_expected<T>::value;

} // namespace ouinet
