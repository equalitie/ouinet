#define BOOST_TEST_MODULE http_sign
#include <boost/test/included/unit_test.hpp>

#include <string>

#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>

#include <util.h>
#include <cache/http_sign.h>

#include <namespaces.h>

BOOST_AUTO_TEST_SUITE(ouinet_http_sign)

BOOST_AUTO_TEST_CASE(test_http_sign) {

    using namespace std;
    using namespace ouinet;

    sys::error_code ec;

    const string body = "<!DOCTYPE html>\n<p>Tiny body here!</p>";
    const string b64_digest = util::base64_encode(util::sha256_digest(body));
    BOOST_REQUIRE(b64_digest == "j7uwtB/QQz0FJONbkyEmaqlJwGehJLqWoCO1ceuM30w=");

    const string head_s =
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
        "Server: Apache1\r\n"
        "Content-Type: text/html\r\n"
        "Content-Disposition: inline; filename=\"foo.html\"\r\n"
        "Content-Length: 38\r\n"
        "Server: Apache2\r\n"
        "\r\n";
    http::response_parser<http::string_body> parser;
    parser.put(asio::buffer(head_s.data(), head_s.size()), ec);
    BOOST_REQUIRE(!ec);
    parser.put(asio::buffer(body.data(), body.size()), ec);
    BOOST_REQUIRE(!ec);
    BOOST_REQUIRE(parser.is_done());
    auto head = parser.get().base();

    // TODO: complete

}

BOOST_AUTO_TEST_SUITE_END()
