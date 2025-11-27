#pragma once

#include <string_view>
#include <array>
#include <memory>
#include <boost/asio/spawn.hpp>
#include "../namespaces.h"
#include "yield.h"

// Scrypt password hashing function
// https://docs.openssl.org/1.1.1/man7/scrypt
namespace ouinet::util {

struct ScryptParams {
    uint64_t N;
    uint64_t r;
    uint64_t p;
};

class ScryptWorker {
private:
    struct Impl;

public:
    static ScryptWorker global_worker;

    ScryptWorker();

    template<size_t OutputSize>
    std::array<uint8_t, OutputSize> derive(
            std::string_view password,
            std::string_view salt,
            ScryptParams params,
            YieldContext yield
    ) {
        std::array<uint8_t, OutputSize> output;
        derive(password, salt, params, output.data(), output.size(), yield);
        return output;
    }

private:
    void derive(
            std::string_view password,
            std::string_view salt,
            ScryptParams params,
            uint8_t* output_data,
            size_t output_size,
            YieldContext);

private:
    std::shared_ptr<Impl> _impl;
};

enum ScryptError {
    success = 0,
    init,
    set_N,
    set_r,
    set_p,
    set_pass,
    set_salt,
    derive
};

sys::error_category const& scrypt_error_category();

inline
sys::error_code make_error_code(ScryptError e) {
    return sys::error_code(static_cast<int>( e ), scrypt_error_category());
}

} // namespace ouinet::util

namespace boost::system {
    template<> struct is_error_code_enum< ::ouinet::util::ScryptError >: std::true_type{};
} // namespace boost::system
