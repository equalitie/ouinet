#pragma once

#include <array>
#include <openssl/rand.h>
#include <boost/system/result.hpp>
#include "error.h"

namespace ouinet {

namespace detail {
    template<size_t N>
    boost::system::result<std::array<uint8_t, N>> generate_random_array() {
        std::array<uint8_t, N> array;
        if (RAND_bytes(array.data(), array.size()) != 1) {
            return OuinetError::openssl_failed_to_generate_random_data;
        }
        return array;
    }
}

struct CryptoStreamKey : std::array<uint8_t, 32> {
    static boost::system::result<CryptoStreamKey> generate_random() {
        auto array = detail::generate_random_array<32>();
        if (!array) return array.error();
        return CryptoStreamKey{*array};
    }

    // For testing
    //static CryptoStreamKey test_key() {
    //    return CryptoStreamKey{{
    //        0,0,0,0,0,0,0,0,
    //        0,0,0,0,0,0,0,0,
    //        0,0,0,0,0,0,0,0,
    //        0,0,0,0,0,0,0,0
    //    }};
    //}
};

} // namespace
