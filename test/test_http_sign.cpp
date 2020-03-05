#define BOOST_TEST_MODULE http_sign
#include <boost/test/data/test_case.hpp>
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
#include <util/crypto.h>
#include <util/wait_condition.h>
#include <util/yield.h>
#include <cache/http_sign.h>
#include <response_reader.h>
#include <session.h>

#include <namespaces.h>
#include "connected_pair.h"

using first_last = std::pair<unsigned, unsigned>;
// <https://stackoverflow.com/a/33965517>
namespace boost { namespace test_tools { namespace tt_detail {
    template<>
    struct print_log_value<first_last> {
        void operator()(std::ostream& os, const first_last& p) {
            os << "{" << p.first << ", " << p.second << "}";
        }
    };
}}} // namespace boost::test_tools::tt_detail

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
static const array<string, 3> rs_block_data{
    rs_block0_head + string(rs_block_fill, rs_block_fill_char) + rs_block0_tail,
    rs_block1_head + string(rs_block_fill, rs_block_fill_char) + rs_block1_tail,
    rs_block2,
};
static const string rs_body =
  ( rs_block_data[0]
  + rs_block_data[1]
  + rs_block_data[2]);
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
static const string _rs_status_origin =
    "HTTP/1.1 200 OK\r\n";

static const string _rs_fields_origin = (
    "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
    "Server: Apache1\r\n"
    "Server: Apache2\r\n"
    "Content-Type: text/html\r\n"
    "Content-Disposition: inline; filename=\"foo.html\"\r\n"
);

static const string _rs_head_injection = (
    "X-Ouinet-Version: 3\r\n"
    "X-Ouinet-URI: https://example.com/foo\r\n"
    "X-Ouinet-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310\r\n"
    "X-Ouinet-BSigs: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",size=65536\r\n"
);

static const string _rs_head_sig0 = (
    "X-Ouinet-Sig0: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",created=1516048310,"
    "headers=\"(response-status) (created) "
    "date server content-type content-disposition "
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs\","
    "signature=\"tnVAAW/8FJs2PRgtUEwUYzMxBBlZpd7Lx3iucAt9q5hYXuY5ci9T7nEn7UxyKMGA1ZvnDMDBbs40dO1OQUkdCA==\"\r\n"
);

static const string _rs_head_framing = (
    "Transfer-Encoding: chunked\r\n"
    "Trailer: X-Ouinet-Data-Size, Digest, X-Ouinet-Sig1\r\n"
);

static const string _rs_head_digest = (
    "X-Ouinet-Data-Size: 131076\r\n"
    "Digest: SHA-256=E4RswXyAONCaILm5T/ZezbHI87EKvKIdxURKxiVHwKE=\r\n"
);

static const string _rs_head_sig1 = (
    "X-Ouinet-Sig1: keyId=\"ed25519=DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=\","
    "algorithm=\"hs2019\",created=1516048311,"
    "headers=\"(response-status) (created) "
    "date server content-type content-disposition "
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs "
    "x-ouinet-data-size "
    "digest\","
    "signature=\"h/PmOlFvScNzDAUvV7tLNjoA0A39OL67/9wbfrzqEY7j47IYVe1ipXuhhCfTnPeCyXBKiMlc4BP+nf0VmYzoAw==\"\r\n"
);

static const string rs_head_signed_s =
    ( _rs_status_origin
    + _rs_fields_origin
    + _rs_head_injection
    + _rs_head_sig0
    + _rs_head_framing
    + _rs_head_digest
    + _rs_head_sig1
    + "\r\n");

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

static const array<string, 4> rs_chunk_ext{
    "",
    rs_block_sig_cx[0],
    rs_block_sig_cx[1],
    rs_block_sig_cx[2],
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

// Send the signed response with all signature headers at the initial head
// (i.e. no trailers).
BOOST_AUTO_TEST_CASE(test_http_flush_verified_no_trailer) {
    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(ctx);

        asio::ip::tcp::socket
            signed_w(ctx), signed_r(ctx),
            hashed_w(ctx), hashed_r(ctx),
            tested_w(ctx), tested_r(ctx);
        tie(signed_w, signed_r) = util::connected_pair(ctx, yield);
        tie(hashed_w, hashed_r) = util::connected_pair(ctx, yield);
        tie(tested_w, tested_r) = util::connected_pair(ctx, yield);

        // Send signed response.
        asio::spawn(ctx, [&signed_w, lock = wc.lock()] (auto y) {
            // Head (raw).  With trailers as normal headers.
            auto trh_start = rs_head_signed_s.find("Trailer:");
            BOOST_REQUIRE(trh_start != string::npos);
            auto trh_end = rs_head_signed_s.find("\r\n", trh_start);
            BOOST_REQUIRE(trh_start != string::npos);
            auto rs_head = rs_head_signed_s;
            rs_head.erase(trh_start, trh_end - trh_start + 2);  // remove "Trailer: ...\r\n"
            asio::async_write( signed_w
                             , asio::const_buffer(rs_head.data(), rs_head.size())
                             , y);

            // Chunk headers and bodies (one chunk per block).
            unsigned bi;
            for (bi = 0; bi < rs_block_data.size(); ++bi) {
                auto cbd = util::bytes::to_vector<uint8_t>(rs_block_data[bi]);
                auto ch = http_response::ChunkHdr(cbd.size(), rs_chunk_ext[bi]);
                ch.async_write(signed_w, y);
                auto cb = http_response::ChunkBody(std::move(cbd), 0);
                cb.async_write(signed_w, y);
            }

            // Last chunk and trailer (raw).
            auto chZ = http_response::ChunkHdr(0, rs_chunk_ext[bi]);
            chZ.async_write(signed_w, y);
            http_response::Trailer tr;  // empty, everything was in head
            tr.async_write(signed_w, y);

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

// About the blocks in the requested data range:
//
//     We have: [ 64K ][ 64K ][ 4B ]
//     We want:          [32K][2B]
//     We get:         [ 64K ][ 4B ]
//
static string rs_head_partial(unsigned first_block, unsigned last_block) {
    size_t first = first_block * http_::response_data_block;
    size_t last = ( (last_block * http_::response_data_block)
                  + rs_block_data[last_block].size() - 1);
    return util::str
        ( "HTTP/1.1 206 Partial Content\r\n"
        , _rs_fields_origin
        , _rs_head_injection
        , _rs_head_digest
        , _rs_head_sig1
        , "X-Ouinet-HTTP-Status: 200\r\n"
        , "Content-Range: bytes ", first, '-', last, "/131076\r\n"
        , "Transfer-Encoding: chunked\r\n"
        , "\r\n");
}

// Actually only the first chunk extension with a signature may need the hash.
static const array<string, 4> rs_chunk_ext_partial{
    "",
    rs_block_sig_cx[0] + rs_block_hash_cx[0],
    rs_block_sig_cx[1] + rs_block_hash_cx[1],
    rs_block_sig_cx[2] + rs_block_hash_cx[2],
};

static const first_last block_ranges[] = {
    {0, 0},  // just first block
    {0, 1},  // two first blocks
    {0, 2},  // all blocks
    {1, 2},  // two last blocks
    {2, 2},  // just last block
};

BOOST_DATA_TEST_CASE( test_http_flush_verified_partial
                    , boost::unit_test::data::make(block_ranges), firstb_lastb) {
    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(ctx);

        asio::ip::tcp::socket
            signed_w(ctx), signed_r(ctx),
            tested_w(ctx), tested_r(ctx);
        tie(signed_w, signed_r) = util::connected_pair(ctx, yield);
        tie(tested_w, tested_r) = util::connected_pair(ctx, yield);

        unsigned first_block, last_block;
        tie(first_block, last_block) = firstb_lastb;

        // Send partial response.
        asio::spawn(ctx, [ &signed_w
                         , first_block, last_block
                         , lock = wc.lock()] (auto y) {
            // Head (raw).
            auto rsp_head = rs_head_partial(first_block, last_block);
            asio::async_write( signed_w
                             , asio::const_buffer(rsp_head.data(), rsp_head.size())
                             , y);

            // Chunk headers and bodies (one chunk per block).
            // We start on the first block of the partial range.
            bool first_chunk = true;
            unsigned bi;
            for (bi = first_block; bi <= last_block; ++bi, first_chunk=false) {
                auto cbd = util::bytes::to_vector<uint8_t>(rs_block_data[bi]);
                auto ch = http_response::ChunkHdr( cbd.size()
                                                 , first_chunk ? "" : rs_chunk_ext_partial[bi]);
                ch.async_write(signed_w, y);
                auto cb = http_response::ChunkBody(std::move(cbd), 0);
                cb.async_write(signed_w, y);
            }

            // Last chunk and empty trailer.
            auto chZ = http_response::ChunkHdr(0, rs_chunk_ext_partial[bi]);
            chZ.async_write(signed_w, y);
            auto tr = http_response::Trailer();
            tr.async_write(signed_w, y);

            signed_w.close();
        });

        // Test the loaded response.
        asio::spawn(ctx, [ signed_r = std::move(signed_r), &tested_w
                         , lock = wc.lock()] (auto y) mutable {
            Cancel cancel;
            sys::error_code e;
            auto pk = get_public_key();
            Session::reader_uptr signed_rvr = make_unique<cache::VerifyingReader>
                ( move(signed_r), pk
                , cache::VerifyingReader::status_set{http::status::partial_content});
            auto signed_rs = Session::create(move(signed_rvr), cancel, y[e]);
            BOOST_REQUIRE_EQUAL(e.message(), "Success");
            signed_rs.flush_response(tested_w, cancel, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
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
