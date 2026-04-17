#define BOOST_TEST_MODULE utility

#include <boost/test/tools/interface.hpp>
#include <boost/test/tools/old/interface.hpp>
#include <boost/test/included/unit_test.hpp>
#include "ouiservice/i2p/address.h"
using ouinet::ouiservice::i2poui::isValidI2PB32;

BOOST_AUTO_TEST_CASE(valid_b32_addresses) {
    // 52-char valid (example: 52 'a's)
    std::string valid52(52, 'a');
    valid52 += ".b32.i2p";
    BOOST_CHECK(isValidI2PB32(valid52));

    // 56-char valid (example: 56 'z's and '2'..'7' allowed)
    std::string valid56 = std::string(56, 'z') + ".b32.i2p";
    BOOST_CHECK(isValidI2PB32(valid56));

    // Mixed base32 chars
    std::string mix52 = std::string("abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvwxyz")
                        .substr(0,52) + ".b32.i2p";
    BOOST_CHECK(isValidI2PB32(mix52));
}

BOOST_AUTO_TEST_CASE(invalid_b32_addresses) {
    // A hostname
    std::string jumble="mewmew.com";
    BOOST_REQUIRE(!isValidI2PB32("mewmew.com"));

    // An ip address
    std::string ip="mewmew.com";
    BOOST_REQUIRE(!isValidI2PB32("127.0.0.1"));

    // base32 garbage
    std::string b32="sdifuosimciwiruwomksla3334";
    BOOST_REQUIRE(!isValidI2PB32(b32));

    // No b32
    std::string wrong_suffix(52, 'a');
    wrong_suffix += ".i2p";
    BOOST_REQUIRE(!isValidI2PB32(wrong_suffix));

    // Too short
    std::string too_short(51, 'a');
    too_short += ".b32.i2p";
    BOOST_REQUIRE(!isValidI2PB32(too_short));

    // Too long
    std::string too_long(57, 'a');
    too_long += ".b32.i2p";
    BOOST_REQUIRE(!isValidI2PB32(too_long));

    // The length which is just incorrect
    std::string incorrect_length(55, 'a');
    incorrect_length += ".b32.i2p";
    BOOST_REQUIRE(!isValidI2PB32(incorrect_length));

    // Invalid characters (uppercase)
    std::string upper52 = std::string(52, 'A') + ".b32.i2p";
    BOOST_REQUIRE(!isValidI2PB32(upper52));

    // Invalid characters (not base32)
    std::string invalid_chars = std::string(52, '0') + ".b32.i2p"; // '0' not allowed
    BOOST_REQUIRE(!isValidI2PB32(invalid_chars));
    invalid_chars = std::string(52, '~') + ".b32.i2p";
    BOOST_REQUIRE(!isValidI2PB32(invalid_chars));
    invalid_chars = std::string(52, '.') + ".b32.i2p";
    BOOST_REQUIRE(!isValidI2PB32(invalid_chars));

}

