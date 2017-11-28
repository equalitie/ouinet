#pragma once

#include <string>

#include <boost/system/error_code.hpp>

namespace ouinet { namespace error {
    enum error_t {
        // 0 means success
        no_more_routes = 1,
    };

    struct ouinet_category : public boost::system::error_category {
        const char* name() const noexcept override {
            return "ouinet_errors";
        }

        std::string message(int e) const {
            switch (e) {
                case error::no_more_routes:
                    return "no more routing mechanisms can be attempted";
                default:
                    return "unknown ouinet error";
            }
        }
    };

    inline
    boost::system::error_code
    make_error_code(::ouinet::error::error_t e) {
        static ouinet_category c;
        return boost::system::error_code(static_cast<int>(e), c);
    }
}} // ouinet::error namespace
