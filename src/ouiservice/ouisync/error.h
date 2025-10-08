#pragma once

#include <boost/system/system_error.hpp>

namespace ouinet::ouisync_service {

enum Error {
    // TODO: This one should be in cache/
    request_to_cache_key,
};

class ErrorCategory : public boost::system::error_category {
public:
    /**
     * Describe category
     */
    const char* name() const noexcept override {
        return "ouisync::ErrorCategory";
    }

    /**
     * Get error message
     */
    std::string message(int ev) const override {
        switch (static_cast<Error>(ev)) {
            default: return "Unknown error";
        }
    }

    /**
     * Map to error condition
     */
    boost::system::error_condition default_error_condition(int ev) const noexcept override {
        // TODO: Map certain errors to a generic condition
        switch (static_cast<Error>(ev)) {
            default:
                return boost::system::error_condition(ev, *this);
        }
    }
};

const boost::system::error_category& error_category();

inline
boost::system::error_code make_error_code(Error e) {
    return {static_cast<int>(e), error_category()};
}

template<typename ErrorCode>
[[noreturn]]
inline void throw_error(
    ErrorCode ec_,
    std::string message = {},
    const boost::source_location& loc = BOOST_CURRENT_LOCATION
) {
    auto ec = make_error_code(ec_);
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

