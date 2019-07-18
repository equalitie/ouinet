#define BOOST_TEST_MODULE http_sign
#include <boost/test/included/unit_test.hpp>

#include <string>

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>

#include <util.h>
#include <util/bytes.h>
#include <cache/http_sign.h>

#include <namespaces.h>

BOOST_AUTO_TEST_SUITE(ouinet_http_sign)

BOOST_AUTO_TEST_CASE(test_http_sign) {

    using namespace std;
    using namespace ouinet;

    sys::error_code ec;

    const string body = "<!DOCTYPE html>\n<p>Tiny body here!</p>";
    const auto digest = util::sha256_digest(body);
    const auto b64_digest = util::base64_encode(digest);
    BOOST_REQUIRE(b64_digest == "j7uwtB/QQz0FJONbkyEmaqlJwGehJLqWoCO1ceuM30w=");

    const string head_s = (
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
        "Server: Apache1\r\n"
        "Content-Type: text/html\r\n"
        "Content-Disposition: inline; filename=\"foo.html\"\r\n"
        "Content-Length: 38\r\n"
        "Server: Apache2\r\n"
        "\r\n"
    );
    http::response_parser<http::string_body> parser;
    parser.put(asio::buffer(head_s), ec);
    BOOST_REQUIRE(!ec);
    parser.put(asio::buffer(body), ec);
    BOOST_REQUIRE(!ec);
    BOOST_REQUIRE(parser.is_done());
    auto head = parser.get().base();

    http::request_header<> req_h;
    req_h.method(http::verb::get);
    req_h.target("https://example.com/foo");  // proxy-like
    req_h.version(11);
    req_h.set(http::field::host, "example.com");

    const string id = "d6076384-2295-462b-a047-fe2c9274e58d";
    const std::chrono::seconds::rep ts = 1516048310;

    head = cache::http_injection_head(req_h, std::move(head), id, ts);

    const auto b64sk = "MfWAV5YllPAPeMuLXwN2mUkV9YaSSJVUcj/2YOaFmwQ=";
    const auto ska = util::bytes::to_array<uint8_t, 32>(util::base64_decode(b64sk));
    const util::Ed25519PrivateKey sk(std::move(ska));
    const auto key_id = cache::http_key_id_for_injection(sk.public_key());
    BOOST_REQUIRE(key_id == "ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=");

    http::fields trailer;
    trailer = cache::http_injection_trailer( head, std::move(trailer)
                                           , body.size(), digest
                                           , sk, key_id, ts + 1);

    // TODO: complete

}

BOOST_AUTO_TEST_SUITE_END()
