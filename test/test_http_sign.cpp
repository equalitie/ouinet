#define BOOST_TEST_MODULE http_sign
#include <boost/test/included/unit_test.hpp>

#include <sstream>
#include <string>

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>

#include <util.h>
#include <util/bytes.h>
#include <util/crypto.h>
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

    const auto b64sk = "MfWAV5YllPAPeMuLXwN2mUkV9YaSSJVUcj/2YOaFmwQ=";
    const auto ska = util::bytes::to_array<uint8_t, util::Ed25519PrivateKey::key_size>(util::base64_decode(b64sk));
    const util::Ed25519PrivateKey sk(std::move(ska));
    const auto key_id = cache::http_key_id_for_injection(sk.public_key());
    BOOST_REQUIRE(key_id == "ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=");

    head = cache::http_injection_head(req_h, std::move(head), id, ts, sk, key_id);

    http::fields trailer;
    trailer = cache::http_injection_trailer( head, std::move(trailer)
                                           , body.size(), digest
                                           , sk, key_id, ts + 1);
    // Add headers from the trailer to the injection head.
    for (auto& hdr : trailer)
        head.set(hdr.name_string(), hdr.value());
    // Remove framing headers from the injection head
    // (they are never part of a signature).
    head.erase(http::field::content_length);
    head.erase(http::field::transfer_encoding);
    head.erase(http::field::trailer);

    // If Beast changes message representation or shuffles headers,
    // the example will need to be updated,
    // but the signature should stay the same.
    // If comparing the whole head becomes too tricky, just check `X-Ouinet-Sig0`.
    const string signed_head = (
        "HTTP/1.1 200 OK\r\n"
        "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
        "Server: Apache1\r\n"
        "Server: Apache2\r\n"
        "Content-Type: text/html\r\n"
        "Content-Disposition: inline; filename=\"foo.html\"\r\n"

        "X-Ouinet-Version: 0\r\n"
        "X-Ouinet-URI: https://example.com/foo\r\n"
        "X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310\r\n"

        "X-Ouinet-Sig0: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
        "algorithm=\"hs2019\",created=1516048310,"
        "headers=\"(response-status) (created) "
        "date server content-type content-disposition "
        "x-ouinet-version x-ouinet-uri x-ouinet-injection\","
        "signature=\"Aoh7kA8OEkiNIqJ6ewBoB7I8olM0T7JAd0wN1yEaQ6PmTuZAFv7C19NSURSmxiLS6q1Aw/o4wSVCYSgjhdlvDw==\"\r\n"

        "X-Ouinet-Data-Size: 38\r\n"
        "Digest: SHA-256=j7uwtB/QQz0FJONbkyEmaqlJwGehJLqWoCO1ceuM30w=\r\n"

        "X-Ouinet-Sig1: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
        "algorithm=\"hs2019\",created=1516048311,"
        "headers=\"(response-status) (created) "
        "date server content-type content-disposition "
        "x-ouinet-version x-ouinet-uri x-ouinet-injection "
        "x-ouinet-data-size "
        "digest\","
        "signature=\"wpxnlch8wwAgEne8ilmG4HtgMwKjSm063IlF1/TS8FEqEBAES1LEcQmsSGHuPEmFdzJ4JRnERkHFO49gfQG7BQ==\"\r\n"
        "\r\n"
    );

    std::stringstream head_ss;
    head_ss << head;
    BOOST_REQUIRE(head_ss.str() == signed_head);

}

BOOST_AUTO_TEST_SUITE_END()
