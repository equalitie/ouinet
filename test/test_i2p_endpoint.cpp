#define BOOST_TEST_MODULE utility

#include <boost/test/tools/interface.hpp>
#include <boost/test/tools/old/interface.hpp>
#include <boost/test/unit_test.hpp>
#include "ouiservice/i2p/address.h"

using ouinet::I2pAddress;

static const char I2P_BASE_64_ALPHABET[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-~";

BOOST_AUTO_TEST_CASE(valid_b32_addresses) {
    // 52-char valid (example: 52 'a's)
    std::string valid52(52, 'a');
    valid52 += ".b32.i2p";
    BOOST_CHECK(I2pAddress::parse(valid52));

    // 56-char valid (example: 56 'z's and '2'..'7' allowed)
    std::string valid56 = std::string(56, 'z') + ".b32.i2p";
    BOOST_CHECK(I2pAddress::parse(valid56));

    // Mixed base32 chars
    std::string mix52 = std::string("abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvwxyz")
                        .substr(0,52) + ".b32.i2p";
    BOOST_CHECK(I2pAddress::parse(mix52));
}

BOOST_AUTO_TEST_CASE(invalid_b32_addresses) {
    // A hostname
    std::string jumble="mewmew.com";
    BOOST_REQUIRE(!I2pAddress::parse("mewmew.com"));

    // An ip address
    std::string ip="mewmew.com";
    BOOST_REQUIRE(!I2pAddress::parse("127.0.0.1"));

    // base32 garbage
    std::string b32="sdifuosimciwiruwomksla3334";
    BOOST_REQUIRE(!I2pAddress::parse(b32));

    // No b32
    std::string wrong_suffix(52, 'a');
    wrong_suffix += ".i2p";
    BOOST_REQUIRE(!I2pAddress::parse(wrong_suffix));

    // Too short
    std::string too_short(51, 'a');
    too_short += ".b32.i2p";
    BOOST_REQUIRE(!I2pAddress::parse(too_short));

    // The length which is just incorrect
    std::string incorrect_length(55, 'a');
    incorrect_length += ".b32.i2p";
    BOOST_REQUIRE(!I2pAddress::parse(incorrect_length));

    // Invalid characters (uppercase)
    std::string upper52 = std::string(52, 'A') + ".b32.i2p";
    BOOST_REQUIRE(!I2pAddress::parse(upper52));

    // Invalid characters (not base32)
    std::string invalid_chars = std::string(52, '0') + ".b32.i2p"; // '0' not allowed
    BOOST_REQUIRE(!I2pAddress::parse(invalid_chars));
    invalid_chars = std::string(52, '~') + ".b32.i2p";
    BOOST_REQUIRE(!I2pAddress::parse(invalid_chars));
    invalid_chars = std::string(52, '.') + ".b32.i2p";
    BOOST_REQUIRE(!I2pAddress::parse(invalid_chars));

}


static std::string make_b64_label(std::size_t len) {
    std::string out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(I2P_BASE_64_ALPHABET[i % (sizeof(I2P_BASE_64_ALPHABET)-1)]);
    }
    return out;
}


BOOST_AUTO_TEST_CASE(valid_b64_addresses)
{
    // long enough payload, no suffix
    BOOST_REQUIRE(I2pAddress::parse(make_b64_label(516)));
    BOOST_REQUIRE(I2pAddress::parse(make_b64_label(520)));

    // long payload with suffix ".b64.i2p"
    std::string long_and_cool = make_b64_label(600) + ".b64.i2p";
    BOOST_REQUIRE(I2pAddress::parse(long_and_cool));

    // one '=' padding before suffix
    std::string padded = make_b64_label(520) + '=' + ".b64.i2p";
    BOOST_REQUIRE(I2pAddress::parse(padded));

    // multiple '=' padding before suffix
    std::string padded2 = make_b64_label(520) + "==" + ".b64.i2p";
    BOOST_REQUIRE(I2pAddress::parse(padded2));

    // exactly 516 chars then suffix
    std::string minimal = make_b64_label(516) + ".b64.i2p";
    BOOST_REQUIRE(I2pAddress::parse(minimal));
}

BOOST_AUTO_TEST_CASE(invalid_b64_addresses)
{
    // Too short
    BOOST_REQUIRE(!I2pAddress::parse(make_b64_label(515)));

    // Contains disallowed character
    std::string wrong_char = make_b64_label(520);
    wrong_char[10] = '+';
    BOOST_REQUIRE(!I2pAddress::parse(wrong_char));

    // Suffix embedded in middle
    std::string warped = make_b64_label(600);
    warped.insert(100, ".b64.i2p");
    BOOST_REQUIRE(!I2pAddress::parse(warped));

    // '=' in middle of payload
    std::string wrong_padding = make_b64_label(520);
    wrong_padding[5] = '=';
    BOOST_REQUIRE(!I2pAddress::parse(wrong_padding));

    // Wrong-case suffix
    std::string uppercase = make_b64_label(520) + ".B64.I2P";
    BOOST_REQUIRE(!I2pAddress::parse(uppercase));

}
