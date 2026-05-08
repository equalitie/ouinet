#include "error.h"
#include "namespaces.h"

namespace ouinet {

class OuinetErrorCategory: public sys::error_category {
public:
    const char* name() const noexcept {
        return "ouinet error";
    }

    std::string message( int ev ) const {
        char buffer[ 64 ];
        return this->message( ev, buffer, sizeof(buffer));
    }

    char const* message(int ev, char * buffer, std::size_t len) const noexcept {
        switch(static_cast<OuinetError>(ev))
        {
            case OuinetError::success: return "no error";
            case OuinetError::openssl_failed_to_generate_random_data: return "OpenSSL failed to produce random data";
        }

        std::snprintf(buffer, len, "Unknown error %d", ev );
        return buffer;
    }
};

sys::error_category const& ouinet_error_category() {
    static const OuinetErrorCategory instance;
    return instance;
}

} // namespace
