#pragma once

namespace ouinet {

// Utility for using std::variant
// See: https://en.cppreference.com/w/cpp/utility/variant/visit.html
template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

} // namespace
