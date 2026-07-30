#pragma once

namespace ouinet {

// Utility for using std::variant
// See: https://en.cppreference.com/w/cpp/utility/variant/visit.html
//
// Example:
//   std::variant<std::string, int> v;
//
//   std::visit(overloaded {
//           [&] (std::string const& s) {
//               std::cout << s;
//           },
//           [&] (int i) {
//               std::cout << i;
//           },
//       },
//       v);

template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

} // namespace
