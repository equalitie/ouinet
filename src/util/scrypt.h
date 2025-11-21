#pragma once

#include <string_view>
#include <array>
#include <memory>
#include <boost/asio/spawn.hpp>
#include "../namespaces.h"

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
            asio::yield_context yield
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
            asio::yield_context);

private:
    std::shared_ptr<Impl> _impl;
};

} // namespace ouinet::util
