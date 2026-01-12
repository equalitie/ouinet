#include "error.h"

namespace ouinet::ouisync_service {

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

const boost::system::error_category& error_category() {
    static ErrorCategory instance;
    return instance;
}

} // namespace ouinet::ouisync_service
