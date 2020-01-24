#define BOOST_TEST_MODULE http_sign
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <sstream>
#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>

#include <util.h>
#include <util/bytes.h>
#include <util/connected_pair.h>
#include <util/crypto.h>
#include <util/wait_condition.h>
#include <util/yield.h>
#include <cache/http_sign.h>
#include <response_reader.h>
#include <session.h>

#include <namespaces.h>

BOOST_AUTO_TEST_SUITE(ouinet_http_sign)

using namespace std;
using namespace ouinet;

static const string rq_target = "https://example.com/foo";  // proxy-like
static const string rq_host = "example.com";

static const string rs_block0_head = "0123";
static const string rs_block0_tail = "4567";
static const string rs_block1_head = "89AB";
static const string rs_block1_tail = "CDEF";
static const string rs_block2 = "abcd";
static const char rs_block_fill_char = 'x';
static const size_t rs_block_fill = ( http_::response_data_block
                                    - rs_block0_head.size()
                                    - rs_block0_tail.size());
static const string rs_body =
  ( rs_block0_head + string(rs_block_fill, rs_block_fill_char) + rs_block0_tail
  + rs_block1_head + string(rs_block_fill, rs_block_fill_char) + rs_block1_tail
  + rs_block2);
static const string rs_body_b64digest = "E4RswXyAONCaILm5T/ZezbHI87EKvKIdxURKxiVHwKE=";
static const string rs_head_s = (
    "HTTP/1.1 200 OK\r\n"
    "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
    "Server: Apache1\r\n"
    "Content-Type: text/html\r\n"
    "Content-Disposition: inline; filename=\"foo.html\"\r\n"
    "Content-Length: 131076\r\n"
    "Server: Apache2\r\n"
    "\r\n"
);
static const string inj_id = "d6076384-2295-462b-a047-fe2c9274e58d";
static const std::chrono::seconds::rep inj_ts = 1516048310;
static const string inj_b64sk = "MfWAV5YllPAPeMuLXwN2mUkV9YaSSJVUcj/2YOaFmwQ=";
static const string inj_b64pk = "DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=";

// If Beast changes message representation or shuffles headers,
// the example will need to be updated,
// but the signature should stay the same.
// If comparing the whole head becomes too tricky, just check `X-Ouinet-Sig0`.
static const string rs_head_signed_s = (
    "HTTP/1.1 200 OK\r\n"
    "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
    "Server: Apache1\r\n"
    "Server: Apache2\r\n"
    "Content-Type: text/html\r\n"
    "Content-Disposition: inline; filename=\"foo.html\"\r\n"

    "X-Ouinet-Version: 3\r\n"
    "X-Ouinet-URI: https://example.com/foo\r\n"
    "X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310\r\n"
    "X-Ouinet-BSigs: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",size=65536\r\n"

    "X-Ouinet-Sig0: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",created=1516048310,"
    "headers=\"(response-status) (created) "
    "date server content-type content-disposition "
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs\","
    "signature=\"tnVAAW/8FJs2PRgtUEwUYzMxBBlZpd7Lx3iucAt9q5hYXuY5ci9T7nEn7UxyKMGA1ZvnDMDBbs40dO1OQUkdCA==\"\r\n"

    "Transfer-Encoding: chunked\r\n"
    "Trailer: X-Ouinet-Data-Size, Digest, X-Ouinet-Sig1\r\n"

    "X-Ouinet-Data-Size: 131076\r\n"
    "Digest: SHA-256=E4RswXyAONCaILm5T/ZezbHI87EKvKIdxURKxiVHwKE=\r\n"

    "X-Ouinet-Sig1: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",created=1516048311,"
    "headers=\"(response-status) (created) "
    "date server content-type content-disposition "
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs "
    "x-ouinet-data-size "
    "digest\","
    "signature=\"h/PmOlFvScNzDAUvV7tLNjoA0A39OL67/9wbfrzqEY7j47IYVe1ipXuhhCfTnPeCyXBKiMlc4BP+nf0VmYzoAw==\"\r\n"
    "\r\n"
);

static const array<string, 3> rs_block_hash_cx{
    "",  // no previous block to hash
    ";ouihash=\"aERfr5o+kpvR4ZH7xC0mBJ4QjqPUELDzjmzt14WmntxH2p3EQmATZODXMPoFiXaZL6KNI50Ve4WJf/x3ma4ieA==\"",
    ";ouihash=\"slwciqMQBddB71VWqpba+MpP9tBiyTE/XFmO5I1oiVJy3iFniKRkksbP78hCEWOM6tH31TGEFWP1loa4pqrLww==\"",
};

static const array<string, 3> rs_block_sig_cx{
    ";ouisig=\"AwiYuUjLYh/jZz9d0/ev6dpoWqjU/sUWUmGL36/D9tI30oaqFgQGgcbVCyBtl0a7x4saCmxRHC4JW7cYEPWwCw==\"",
    ";ouisig=\"c+ZJUJI/kc81q8sLMhwe813Zdc+VPa4DejdVkO5ZhdIPPojbZnRt8OMyFMEiQtHYHXrZIK2+pKj2AO03j70TBA==\"",
    ";ouisig=\"m6sz1NpU/8iF6KNN6drY+Yk361GiW0lfa0aaX5TH0GGW/L5GsHyg8ozA0ejm29a+aTjp/qIoI1VrEVj1XG/gDA==\"",
};

template<class F>
static void run_spawned(asio::io_context& ctx, F&& f) {
    asio::spawn(ctx, [&ctx, f = forward<F>(f)] (auto yield) {
            try {
                f(Yield(ctx, yield));
            }
            catch (const std::exception& e) {
                BOOST_ERROR(string("Test ended with exception: ") + e.what());
            }
        });
    ctx.run();
}

static http::request_header<> get_request_header() {
    http::request_header<> req_h;
    req_h.method(http::verb::get);
    req_h.target(rq_target);
    req_h.version(11);
    req_h.set(http::field::host, rq_host);

    return req_h;
}

static util::Ed25519PrivateKey get_private_key() {
    auto ska = util::bytes::to_array<uint8_t, util::Ed25519PrivateKey::key_size>(util::base64_decode(inj_b64sk));
    return util::Ed25519PrivateKey(std::move(ska));
}

static util::Ed25519PublicKey get_public_key() {
    auto pka = util::bytes::to_array<uint8_t, util::Ed25519PublicKey::key_size>(util::base64_decode(inj_b64pk));
    return util::Ed25519PublicKey(std::move(pka));
}

struct TestGlobalFixture {
    void setup() {
        ouinet::util::crypto_init();
    }
};

BOOST_TEST_GLOBAL_FIXTURE(TestGlobalFixture);

BOOST_AUTO_TEST_CASE(test_http_sign) {
    sys::error_code ec;

    const auto digest = util::sha256_digest(rs_body);
    const auto b64_digest = util::base64_encode(digest);
    BOOST_REQUIRE(b64_digest == rs_body_b64digest);

    http::response_parser<http::string_body> parser;
    parser.put(asio::buffer(rs_head_s), ec);
    BOOST_REQUIRE(!ec);
    parser.put(asio::buffer(rs_body), ec);
    BOOST_REQUIRE(!ec);
    BOOST_REQUIRE(parser.is_done());
    auto rs_head = parser.get().base();

    auto req_h = get_request_header();

    const auto sk = get_private_key();
    const auto key_id = cache::http_key_id_for_injection(sk.public_key());
    BOOST_REQUIRE(key_id == ("ed25519=" + inj_b64pk));

    rs_head = cache::http_injection_head(req_h, std::move(rs_head), inj_id, inj_ts, sk, key_id);

    http::fields trailer;
    trailer = cache::http_injection_trailer( rs_head, std::move(trailer)
                                           , rs_body.size(), digest
                                           , sk, key_id, inj_ts + 1);
    // Add headers from the trailer to the injection head.
    for (auto& hdr : trailer)
        rs_head.set(hdr.name_string(), hdr.value());

    std::stringstream rs_head_ss;
    rs_head_ss << rs_head;
    BOOST_REQUIRE(rs_head_ss.str() == rs_head_signed_s);

}

// Put everything in the string to the given parser,
// until everything is parsed or some error happens.
template<class Parser>
static
void put_to_parser(Parser& p, const string& s, sys::error_code& ec) {
    auto b = asio::const_buffer(s.data(), s.size());
    while (b.size() > 0) {
        auto consumed = p.put(b, ec);
        if (ec) return;
        b += consumed;
    };
}

BOOST_AUTO_TEST_CASE(test_http_verify) {
    sys::error_code ec;

    http::response_parser<http::string_body> parser;
    put_to_parser(parser, rs_head_signed_s, ec);
    BOOST_REQUIRE(!ec);
    BOOST_REQUIRE(parser.is_header_done());
    BOOST_REQUIRE(parser.chunked());
    // The signed response head signals chunked transfer encoding.
    auto rs_body_s = ( beast::buffers_to_string(http::make_chunk(asio::buffer(rs_body)))
                     // We should really be adding the trailer here,
                     // but it is already part of `rs_head_signed_s`.
                     // Beast seems to be fine with that, though.
                     + beast::buffers_to_string(http::make_chunk_last()));
    put_to_parser(parser, rs_body_s, ec);
    BOOST_REQUIRE(!ec);
    BOOST_REQUIRE(parser.is_done());
    auto rs_head_signed = parser.get().base();

    const auto pk = get_public_key();
    const auto key_id = cache::http_key_id_for_injection(pk);
    BOOST_REQUIRE(key_id == ("ed25519=" + inj_b64pk));

    // Add an unexpected header.
    // It should not break signature verification, but it should be removed from its output.
    rs_head_signed.set("X-Foo", "bar");
    // Move a header, keeping the same value.
    // It should not break signature verification.
    auto date = rs_head_signed[http::field::date].to_string();
    rs_head_signed.erase(http::field::date);
    rs_head_signed.set(http::field::date, date);

    auto vfy_res = cache::http_injection_verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res.cbegin() != vfy_res.cend());  // successful verification
    BOOST_REQUIRE(vfy_res["X-Foo"].empty());
    // TODO: check same headers

    // Add a bad third signature (by altering the second one).
    // It should not break signature verification, but it should be removed from its output.
    auto sig1_copy = rs_head_signed["X-Ouinet-Sig1"].to_string();
    string sstart(",signature=\"");
    auto spos = sig1_copy.find(sstart);
    BOOST_REQUIRE(spos != string::npos);
    sig1_copy.replace(spos + sstart.length(), 7, "GARBAGE");  // change signature
    rs_head_signed.set("X-Ouinet-Sig2", sig1_copy);

    vfy_res = cache::http_injection_verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res.cbegin() != vfy_res.cend());  // successful verification
    BOOST_REQUIRE(vfy_res["X-Ouinet-Sig2"].empty());

    // Change the key id of the third signature to refer to some other key.
    // It should not break signature verification, and it should be kept in its output.
    auto kpos = sig1_copy.find(inj_b64pk);
    BOOST_REQUIRE(kpos != string::npos);
    sig1_copy.replace(kpos, 7, "GARBAGE");  // change keyId
    rs_head_signed.set("X-Ouinet-Sig2", sig1_copy);

    vfy_res = cache::http_injection_verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res.cbegin() != vfy_res.cend());  // successful verification
    BOOST_REQUIRE(!vfy_res["X-Ouinet-Sig2"].empty());
    // TODO: check same headers

    // Alter the value of one of the signed headers and verify again.
    // It should break signature verification.
    rs_head_signed.set(http::field::server, "NginX");
    vfy_res = cache::http_injection_verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res.cbegin() == vfy_res.cend());  // unsuccessful verification

}

BOOST_AUTO_TEST_CASE(test_http_flush_signed) {
    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(ctx);

        asio::ip::tcp::socket
            origin_w(ctx), origin_r(ctx),
            signed_w(ctx), signed_r(ctx),
            tested_w(ctx), tested_r(ctx);
        tie(origin_w, origin_r) = util::connected_pair(ctx, yield);
        tie(signed_w, signed_r) = util::connected_pair(ctx, yield);
        tie(tested_w, tested_r) = util::connected_pair(ctx, yield);

        // Send raw origin response.
        asio::spawn(ctx, [&origin_w, lock = wc.lock()] (auto y) {
            sys::error_code e;
            asio::async_write( origin_w
                             , asio::const_buffer(rs_head_s.data(), rs_head_s.size())
                             , y[e]);
            BOOST_REQUIRE(!e);
            asio::async_write( origin_w
                             , asio::const_buffer(rs_body.data(), rs_body.size())
                             , y[e]);
            BOOST_REQUIRE(!e);
            origin_w.close();
        });

        // Sign origin response.
        asio::spawn(ctx, [ origin_r = std::move(origin_r), &signed_w
                         , lock = wc.lock()] (auto y) mutable {
            Cancel cancel;
            sys::error_code e;
            auto req_h = get_request_header();
            auto sk = get_private_key();
            Session::reader_uptr origin_rvr = make_unique<cache::SigningReader>
                (move(origin_r), move(req_h), inj_id, inj_ts, sk);
            auto origin_rs = Session::create(std::move(origin_rvr), cancel, y[e]);
            BOOST_REQUIRE(!e);
            origin_rs.flush_response(signed_w, cancel, y[e]);
            BOOST_REQUIRE(!e);
            signed_w.close();
        });

        // Test signed output.
        asio::spawn(ctx, [ signed_r = std::move(signed_r), &tested_w
                         , lock = wc.lock()](auto y) mutable {
            int xidx = 0;
            Cancel cancel;
            sys::error_code e;
            http_response::Reader rr(std::move(signed_r));
            while (true) {
                auto opt_part = rr.async_read_part(cancel, y[e]);
                BOOST_REQUIRE(!e);
                if (!opt_part) break;
                if (auto inh = opt_part->as_head()) {
                    auto hbsh = (*inh)[http_::response_block_signatures_hdr];
                    BOOST_REQUIRE(!hbsh.empty());
                    auto hbs = cache::HttpBlockSigs::parse(hbsh);
                    BOOST_REQUIRE(hbs);
                    // Test data block signatures are split according to this size.
                    BOOST_CHECK_EQUAL(hbs->size, 65536);
                } else if (auto ch = opt_part->as_chunk_hdr()) {
                    if (!ch->exts.empty()) {
                        BOOST_REQUIRE(xidx < rs_block_sig_cx.size());
                        BOOST_CHECK_EQUAL(ch->exts, rs_block_sig_cx[xidx++]);
                    }
                }
                opt_part->async_write(tested_w, cancel, y[e]);
                BOOST_REQUIRE(!e);
            }
            BOOST_CHECK_EQUAL(xidx, rs_block_sig_cx.size());
            tested_w.close();
        });

        // Black hole.
        asio::spawn(ctx, [&tested_r, lock = wc.lock()] (auto y) {
            char d[2048];
            asio::mutable_buffer b(d, sizeof(d));

            sys::error_code e;
            while (!e) asio::async_read(tested_r, b, y[e]);
            BOOST_REQUIRE(e == asio::error::eof || !e);
            tested_r.close();
        });

        wc.wait(yield);
    });
}

BOOST_AUTO_TEST_CASE(test_http_flush_verified) {
    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(ctx);

        asio::ip::tcp::socket
            origin_w(ctx), origin_r(ctx),
            signed_w(ctx), signed_r(ctx),
            hashed_w(ctx), hashed_r(ctx),
            tested_w(ctx), tested_r(ctx);
        tie(origin_w, origin_r) = util::connected_pair(ctx, yield);
        tie(signed_w, signed_r) = util::connected_pair(ctx, yield);
        tie(hashed_w, hashed_r) = util::connected_pair(ctx, yield);
        tie(tested_w, tested_r) = util::connected_pair(ctx, yield);

        // Send raw origin response.
        asio::spawn(ctx, [&origin_w, lock = wc.lock()] (auto y) {
            sys::error_code e;
            asio::async_write( origin_w
                             , asio::const_buffer(rs_head_s.data(), rs_head_s.size())
                             , y[e]);
            BOOST_REQUIRE(!e);
            asio::async_write( origin_w
                             , asio::const_buffer(rs_body.data(), rs_body.size())
                             , y[e]);
            BOOST_REQUIRE(!e);
            origin_w.close();
        });

        // Sign origin response.
        asio::spawn(ctx, [ origin_r = std::move(origin_r), &signed_w
                         , lock = wc.lock()] (auto y) mutable {
            Cancel cancel;
            sys::error_code e;
            auto req_h = get_request_header();
            auto sk = get_private_key();
            Session::reader_uptr origin_rvr = make_unique<cache::SigningReader>
                (move(origin_r), move(req_h), inj_id, inj_ts, sk);
            auto origin_rs = Session::create(std::move(origin_rvr), cancel, y[e]);
            BOOST_REQUIRE(!e);
            origin_rs.flush_response(signed_w, cancel, y[e]);
            BOOST_REQUIRE(!e);
            signed_w.close();
        });

        // Verify signed output.
        asio::spawn(ctx, [ signed_r = std::move(signed_r), &hashed_w
                         , lock = wc.lock()](auto y) mutable {
            Cancel cancel;
            sys::error_code e;
            auto pk = get_public_key();
            Session::reader_uptr signed_rvr = make_unique<cache::VerifyingReader>
                (move(signed_r), pk);
            auto signed_rs = Session::create(move(signed_rvr), cancel, y[e]);
            BOOST_REQUIRE(!e);
            signed_rs.flush_response(hashed_w, cancel, y[e]);
            BOOST_REQUIRE(!e);
            hashed_w.close();
        });

        // Check generation of chained hashes.
        asio::spawn(ctx, [ hashed_r = std::move(hashed_r), &tested_w
                         , &ctx, lock = wc.lock()](auto y) mutable {
            int xidx = 0;
            Cancel cancel;
            sys::error_code e;
            http_response::Reader rr(std::move(hashed_r));
            while (true) {
                auto opt_part = rr.async_read_part(cancel, y[e]);
                BOOST_REQUIRE(!e);
                if (!opt_part) break;
                if (auto ch = opt_part->as_chunk_hdr()) {
                    if (!ch->exts.empty()) {
                        BOOST_REQUIRE(xidx < rs_block_hash_cx.size());
                        BOOST_CHECK(ch->exts.find(rs_block_hash_cx[xidx++]) != string::npos);
                    }
                }
                opt_part->async_write(tested_w, cancel, y[e]);
                BOOST_REQUIRE(!e);
            }
            BOOST_CHECK_EQUAL(xidx, rs_block_hash_cx.size());
            tested_w.close();
        });

        // Black hole.
        asio::spawn(ctx, [&tested_r, lock = wc.lock()] (auto y) {
            char d[2048];
            asio::mutable_buffer b(d, sizeof(d));

            sys::error_code e;
            while (!e) asio::async_read(tested_r, b, y[e]);
            BOOST_REQUIRE(e == asio::error::eof || !e);
            tested_r.close();
        });

        wc.wait(yield);
    });
}

BOOST_AUTO_TEST_CASE(test_http_flush_forged) {
    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(ctx);

        asio::ip::tcp::socket
            origin_w(ctx), origin_r(ctx),
            signed_w(ctx), signed_r(ctx),
            forged_w(ctx), forged_r(ctx),
            tested_w(ctx), tested_r(ctx);
        tie(origin_w, origin_r) = util::connected_pair(ctx, yield);
        tie(signed_w, signed_r) = util::connected_pair(ctx, yield);
        tie(forged_w, forged_r) = util::connected_pair(ctx, yield);
        tie(tested_w, tested_r) = util::connected_pair(ctx, yield);

        // Send raw origin response.
        asio::spawn(ctx, [&origin_w, lock = wc.lock()] (auto y) {
            sys::error_code e;
            asio::async_write( origin_w
                             , asio::const_buffer(rs_head_s.data(), rs_head_s.size())
                             , y[e]);
            BOOST_REQUIRE(!e);
            asio::async_write( origin_w
                             , asio::const_buffer(rs_body.data(), rs_body.size())
                             , y[e]);
            BOOST_REQUIRE(!e);
            origin_w.close();
        });

        // Sign origin response.
        asio::spawn(ctx, [ origin_r = std::move(origin_r), &signed_w
                         , lock = wc.lock()] (auto y) mutable {
            Cancel cancel;
            sys::error_code e;
            auto req_h = get_request_header();
            auto sk = get_private_key();
            Session::reader_uptr origin_rvr = make_unique<cache::SigningReader>
                (move(origin_r), move(req_h), inj_id, inj_ts, sk);
            auto origin_rs = Session::create(std::move(origin_rvr), cancel, y[e]);
            BOOST_REQUIRE(!e);
            origin_rs.flush_response(signed_w, cancel, y[e]);
            BOOST_REQUIRE(!e);
            signed_w.close();
        });

        // Forge (alter) signed output.
        asio::spawn(ctx, [ &signed_r, &forged_w
                         , lock = wc.lock()] (auto y) {
            char d[2048];
            asio::mutable_buffer b(d, sizeof(d));
            auto bsv = util::bytes::to_string_view(b);

            sys::error_code er, ew;
            while (er != asio::error::eof && ew != asio::error::eof) {
                auto l = signed_r.async_read_some(b, y[er]);
                if (er && er != asio::error::eof) break;

                // Alter forwarded content somewhere in the second data block.
                auto rci = bsv.find(rs_block1_tail);
                if (rci != string::npos)
                    d[rci] = rs_block1_tail[0] + 1;

                asio::async_write(forged_w, asio::buffer(b, l), y[ew]);
                if (ew && ew != asio::error::eof) break;
            }
            BOOST_REQUIRE(er == asio::error::eof || !er);
            BOOST_REQUIRE(ew == asio::error::eof || !ew);
            signed_r.close();
            forged_w.close();
        });

        // Verify forged output.
        asio::spawn(ctx, [ forged_r = std::move(forged_r), &tested_w
                         , lock = wc.lock()](auto y) mutable {
            Cancel cancel;
            sys::error_code e;
            auto pk = get_public_key();
            Session::reader_uptr forged_rvr = make_unique<cache::VerifyingReader>
                (move(forged_r), pk);
            auto forged_rs = Session::create(move(forged_rvr), cancel, y[e]);
            BOOST_REQUIRE(!e);
            forged_rs.flush_response(tested_w, cancel, y[e]);
            BOOST_CHECK_EQUAL(e.value(), sys::errc::bad_message);
            tested_w.close();
        });

        // Black hole.
        asio::spawn(ctx, [&tested_r, lock = wc.lock()] (auto y) {
            char d[2048];
            asio::mutable_buffer b(d, sizeof(d));

            sys::error_code e;
            while (!e) asio::async_read(tested_r, b, y[e]);
            BOOST_REQUIRE(e == asio::error::eof || !e);
            tested_r.close();
        });

        wc.wait(yield);
    });
}

BOOST_AUTO_TEST_SUITE_END()
