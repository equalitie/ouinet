#pragma once

#include <array>
#include <openssl/rand.h>

namespace ouinet {

namespace detail {
    template<size_t N>
    std::array<uint8_t, N> generate_random_array() {
        std::array<uint8_t, N> array;
        if (RAND_bytes(array.data(), array.size()) != 1) {
            throw std::runtime_error("RAND_bytes failed");
        }
        return array;
    }
}

struct CryptoStreamKey : std::array<uint8_t, 32> {
    static CryptoStreamKey generate_random() {
        return CryptoStreamKey{detail::generate_random_array<32>()};
    }

    static CryptoStreamKey test_key() {
        return CryptoStreamKey{{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}};
    }
};

} // namespace
