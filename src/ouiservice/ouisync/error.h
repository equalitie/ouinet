#pragma once

#include <boost/system/system_error.hpp>

namespace ouinet::ouisync_service {

enum Error {
    // TODO:
};

const boost::system::error_category& error_category();

inline
boost::system::error_code make_error_code(Error e) {
    return {static_cast<int>(e), error_category()};
}

[[noreturn]]
inline void throw_error(
    boost::system::error_code ec,
    std::string message = {},
    const boost::source_location& loc = BOOST_CURRENT_LOCATION
) {
    ec.assign(ec, &loc);
    boost::system::system_error e(ec, std::move(message));
    throw e;
}

} // namespace ouinet::ouisync_service

namespace boost::system {
    template<> struct is_error_code_enum<ouinet::ouisync_service::Error>
    {
      static const bool value = true;
    };
} // namespace boost::system

