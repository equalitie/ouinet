#pragma once

#include <boost/system/error_category.hpp>

namespace ouinet {

// For errors originating from Ouinet
enum class OuinetError {
    success = 0,
    openssl_failed_to_generate_random_data,
};

boost::system::error_category const& ouinet_error_category();

inline
boost::system::error_code make_error_code(OuinetError e) {
    return boost::system::error_code(static_cast<int>(e), ouinet_error_category());
}

} // namespace ouinet

namespace boost::system {
    template<> struct is_error_code_enum<::ouinet::OuinetError>: std::true_type{};
} // namespace boost::system
