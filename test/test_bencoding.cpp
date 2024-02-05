#define BOOST_TEST_MODULE bencoding
#include <boost/test/included/unit_test.hpp>

#include <bittorrent/bencoding.h>

BOOST_AUTO_TEST_SUITE(bencoding)

using boost::optional;
using ouinet::bittorrent::bencoding_encode;
using ouinet::bittorrent::bencoding_decode;

BOOST_AUTO_TEST_CASE(test_bencoding)
{
    std::string bencoded;

    bencoded = bencoding_encode("");
    BOOST_REQUIRE_EQUAL("0:", bencoded);

    bencoded = bencoding_encode(123);
    BOOST_REQUIRE_EQUAL("i123e", bencoded);

    ouinet::bittorrent::BencodedMap bmap = {{"one", 1}, {"two", 2}};
    bencoded = bencoding_encode(bmap);
    BOOST_REQUIRE_EQUAL("d3:onei1e3:twoi2ee", bencoded);
}

BOOST_AUTO_TEST_CASE(test_bencoding_special_chars)
{
    std::string bencoded;

    bencoded = bencoding_encode("\"");
    BOOST_REQUIRE_EQUAL("1:\"", bencoded);

    ouinet::bittorrent::BencodedMap bmap = {{"a", "\""}};
    bencoded = bencoding_encode(bmap);
    BOOST_REQUIRE_EQUAL("d1:a1:\"e", bencoded);
}

BOOST_AUTO_TEST_CASE(test_decoding)
{
    auto decoded_value_1 = bencoding_decode("3:abc");
    BOOST_REQUIRE_EQUAL("abc", *(decoded_value_1->as_string()));
    // Sending the decoded values to cout validates that the overloaded insertion operator is properly implemented
    std::cout << *decoded_value_1;

    auto decoded_value_2 = bencoding_decode("d3:onei1e3:twoi2ee");
    auto decoded_map = decoded_value_2->as_map();
    BOOST_REQUIRE_EQUAL(1, *(decoded_map->at("one").as_int()));
    BOOST_REQUIRE_EQUAL(2, *(decoded_map->at("two").as_int()));
    std::cout << *decoded_value_2;

    auto decoded_value_3 = bencoding_decode("l4:abcd4:wxyze");
    auto decoded_list = decoded_value_3->as_list();
    BOOST_REQUIRE_EQUAL("abcd", decoded_list->at(0));
    BOOST_REQUIRE_EQUAL("wxyz", decoded_list->at(1));
    std::cout << *decoded_value_3;
}

BOOST_AUTO_TEST_SUITE_END()