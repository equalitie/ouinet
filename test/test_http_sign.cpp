#define BOOST_TEST_MODULE http_sign
#include <boost/test/data/test_case.hpp>
#include <boost/test/unit_test.hpp>

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
#include <util/wait_condition.h>
#include <cache/http_sign.h>
#include <cache/chain_hasher.h>
#include <cache/signed_head.h>
#include <response_reader.h>
#include <session.h>

#include <constants.h>
#include <namespaces.h>
#include "connected_pair.h"
#include "util/unwrap.h"
#include "util/str.h"

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

static const string rs_body_empty = "";
static const string rs_body_b64digest_empty = "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=";

static string _get_response_head(size_t body_size) {
    return  util::str(
    "HTTP/1.1 200 OK\r\n"
    "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
    "Server: Apache1\r\n"
    "Content-Type: text/html\r\n"
    "Content-Disposition: inline; filename=\"foo.html\"\r\n"
    "Content-Length: ",body_size,"\r\n"
    "Server: Apache2\r\n"
    "\r\n");
}

static const auto rs_head_s = _get_response_head(rs_body.size());
static const auto rs_head_s_empty = _get_response_head(rs_body_empty.size());

static const string inj_id = "d6076384-2295-462b-a047-fe2c9274e58d";
static const std::chrono::seconds::rep inj_ts = 1516048310;
static const size_t inj_bs = 65536;
static const string inj_b64sk = "MfWAV5YllPAPeMuLXwN2mUkV9YaSSJVUcj/2YOaFmwQ=";
static const string inj_b64pk = "DlBwx8WbSsZP7eni20bf5VKUH3t1XAF/+hlDoLbZzuw=";

static sign::SecretKey get_private_key() {
    auto ska = util::bytes::to_array<uint8_t, sign::SecretKey::size>(util::base64_decode(inj_b64sk));
    return sign::SecretKey(ska);
}

static sign::PublicKey get_public_key() {
    auto pka = util::bytes::to_array<uint8_t, sign::PublicKey::size>(util::base64_decode(inj_b64pk));
    return sign::PublicKey(std::move(pka));
}

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

static const string _rs_head_injection = util::str(
    "X-Ouinet-Version: ",http_::protocol_version_current,"\r\n",
    "X-Ouinet-URI: ",rq_target,"\r\n",
    "X-Ouinet-Injection: id=", inj_id, ",ts=", inj_ts, "\r\n",
    "X-Ouinet-BSigs: keyId=\"ed25519=",inj_b64pk,"\",",
    "algorithm=\"hs2019\",size=",inj_bs,"\r\n"
);

static string _get_signature_field(bool is_final, size_t body_size, const string& body_b64digest) {
    auto sig_ts = is_final ? inj_ts+1 : inj_ts;
    return util::str(
    "X-Ouinet-Sig", is_final ? 1 : 0, ": "
    "keyId=\"ed25519=",inj_b64pk,"\",",
    "algorithm=\"hs2019\",created=",sig_ts,",",
    "headers=\"(response-status) (created) ",
    "date server content-type content-disposition ",
    "x-ouinet-version x-ouinet-uri x-ouinet-injection x-ouinet-bsigs",
    is_final ? " x-ouinet-data-size digest" : "", "\",",
    "signature=\"",util::base64_encode(get_private_key().sign(util::str(
            "(response-status): 200\n"
            "(created): ",sig_ts,"\n"
            "date: Mon, 15 Jan 2018 20:31:50 GMT\n"
            "server: Apache1, Apache2\n"
            "content-type: text/html\n"
            "content-disposition: inline; filename=\"foo.html\"\n"
            "x-ouinet-version: ",http_::protocol_version_current,"\n"
            "x-ouinet-uri: ",rq_target,"\n"
            "x-ouinet-injection: id=",inj_id,",ts=",inj_ts,"\n"
            "x-ouinet-bsigs: keyId=\"ed25519=",inj_b64pk,"\",algorithm=\"hs2019\",size=",inj_bs,
            is_final ? util::str( "\n"
                                  "x-ouinet-data-size: ", body_size, "\n"
                                  "digest: SHA-256=", body_b64digest)
                     : "")).bytes), "\"",
    "\r\n" );
}

static const string _rs_head_framing = (
    "Transfer-Encoding: chunked\r\n"
    "Trailer: X-Ouinet-Data-Size, Digest, X-Ouinet-Sig1\r\n"
);

static string _get_digest_fields(size_t body_size, const string& body_b64digest) {
    return util::str(
    "X-Ouinet-Data-Size: ",body_size,"\r\n"
    "Digest: SHA-256=",body_b64digest,"\r\n"
    );
}

static string get_signed_response_head(size_t body_size, const string& body_b64digest) {
    return util::str
    ( _rs_status_origin
    , _rs_fields_origin
    , _rs_head_injection
    , _get_signature_field(false, body_size, body_b64digest)
    , _rs_head_framing
    , _get_digest_fields(body_size, body_b64digest)
    , _get_signature_field(true, body_size, body_b64digest)
    , "\r\n");
}

static const auto rs_head_signed_s = get_signed_response_head(rs_body.size(), rs_body_b64digest);
static const auto rs_head_signed_s_empty = get_signed_response_head(rs_body_empty.size(), rs_body_b64digest_empty);

// As they appear in chunk extensions following a data block.
static const array<string, 3> rs_block_hash_cx{
    "",  // no previous block to hash
    ";ouihash=\"4c0RNY1zc7KD7WqcgnEnGv2BJPLDLZ8ie8/kxtwBLoN2LJNnzUMFzXZoYy1NnddokpIxEm3dL+gJ7dr0xViVOg==\"",  // chash[0]
    ";ouihash=\"ELwO/upgGHUv+GGm8uFMqQPtpLpNHUtSsLPuGo7lflgLZGA8GVfrFF1yuNOx1U998iF2rAApn8Yua80Fnn+TKg==\"",  // chash[1]
    //";ouihash=\"zBvQ0lnfde2B6dRt2B0HvW/kaiL1TXNlbezQmhNqh0zCxMBHb0SWPsWeKNDbsHFdyKzZlauqzVSfAsHer0fq+w==\"",  // chash[2], not sent
};

static const array<string, 1> rs_block_hash_cx_empty{
    "",  // no previous block to hash
};

static const array<string, 3> rs_block_sigs{
    "r2OtBbBVBXT2b8Ch/eFfQt1eDoG8eMs/JQxnjzNPquF80WcUNwQQktsu0mF0+bwc3akKdYdBDeORNLhRjrxVBA==",
    "LfRN72Vv5QMNd6sn6HOWbfcoN6DA9kdjTXEfJvmgViZQZT5hlZXQpCOULyBreeZv3sd7j5FJzgu3CCUoBXOCCA==",
    "oZ3hLELDPOK4y2b0Yd6ezoXaF37PqBXt/WX7YJAzfS4au/QewCQxMlds8qtNWjOrP9Gzyde3jjFn647srWI7DA==",
};

static const array<string, 3> rs_block_sig_cx{
    util::str(";ouisig=\"",rs_block_sigs[0],"\""),
    util::str(";ouisig=\"",rs_block_sigs[1],"\""),
    util::str(";ouisig=\"",rs_block_sigs[2],"\""),
};

static const array<string, 1> rs_block_sig_cx_empty{
    ";ouisig=\"sI1HJC2+BeXy39qqaivr9IrUB8B8dlUm8J3WrYlrH0HmdnfA5DlwIrd00sph3OSrJGw/ATzNbUI3xdTS2kccBQ==\"",
};

static const array<string, 4> rs_chunk_ext{
    "",
    rs_block_sig_cx[0],
    rs_block_sig_cx[1],
    rs_block_sig_cx[2],
};

static const auto rs_chunk_ext_empty = rs_block_sig_cx_empty;

template<class F>
static void run_spawned(asio::io_context& ctx, F&& f) {
    task::spawn_detached(ctx.get_executor(), [f = std::forward<F>(f)] (asio::yield_context yield) {
            try {
                f(Async(yield));
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

BOOST_AUTO_TEST_CASE(test_chain_hasher) {
    using namespace cache;

    ChainHasher chh_sign;
    ChainHasher chh_verif;

    auto sk = get_private_key();
    auto pk = get_public_key();

    ChainHasher::Signer sign{inj_id, sk};

    for (size_t i = 0; i < rs_block_data.size(); ++i) {
        auto block = rs_block_data[i];
        auto block_digest = util::sha512_digest(block);

        ChainHash ch_sign = chh_sign.calculate_block(block.size(), block_digest, sign);

        //cerr << i << " block_digest: " << util::base64_encode(block_digest) << "\n";
        //cerr << i << " chain_digest: " << util::base64_encode(ch_sign.chain_digest) << "\n";
        //cerr << i << " chain_signat: " << util::base64_encode(ch_sign.chain_signature) << "\n";

        BOOST_REQUIRE_EQUAL(rs_block_sigs[i], util::base64_encode(ch_sign.chain_signature.bytes));
        BOOST_REQUIRE(ch_sign.verify(pk, inj_id));

        ChainHash ch_verif = chh_verif.calculate_block(block.size(), block_digest, ch_sign.chain_signature);

        BOOST_REQUIRE(ch_verif.verify(pk, inj_id));
    }
}

void read_until_end(asio::ip::tcp::socket& socket, Async yield) {
    char d[2048];
    asio::mutable_buffer b(d, sizeof(d));

    while (true) {
        if (auto r = asio::async_read(socket, b, yield); !r) {
            BOOST_REQUIRE(r.error() == asio::error::eof);
            break;
        }
    }

    socket.close();
}

static const bool true_false[] = {true, false};

BOOST_DATA_TEST_CASE(test_http_sign, boost::unit_test::data::make(true_false), empty) {
    sys::error_code ec;

    const auto& rs_body_ = empty ? rs_body_empty : rs_body;
    const auto digest = util::sha256_digest(rs_body_);
    const auto b64_digest = util::base64_encode(digest);
    const auto& rs_body_b64digest_ = empty ? rs_body_b64digest_empty : rs_body_b64digest;
    BOOST_REQUIRE(b64_digest == rs_body_b64digest_);

    http::response_parser<http::string_body> parser;
    const auto& rs_head_s_ = empty ? rs_head_s_empty : rs_head_s;
    parser.put(asio::buffer(rs_head_s_), ec);
    BOOST_REQUIRE(!ec);
    if (!empty) {
        parser.put(asio::buffer(rs_body_), ec);
        BOOST_REQUIRE(!ec);
    }
    BOOST_REQUIRE(parser.is_done());
    auto rs_head = parser.get().base();

    auto req_h = get_request_header();

    const auto sk = get_private_key();
    const auto key_id = cache::SignedHead::encode_key_id(sk.public_key());
    BOOST_REQUIRE_EQUAL(key_id, ("ed25519=" + inj_b64pk));

    rs_head = cache::SignedHead::sign_response(req_h, std::move(rs_head), inj_id, inj_ts, sk);

    http::fields trailer;
    trailer = cache::http_injection_trailer( rs_head, std::move(trailer)
                                           , rs_body_.size(), digest
                                           , sk, key_id, inj_ts + 1);
    // Add headers from the trailer to the injection head.
    for (auto& hdr : trailer)
        rs_head.set(hdr.name_string(), hdr.value());

    std::string rs_head_s = util::str(rs_head);

    const auto& rs_head_signed_s_ = empty ? rs_head_signed_s_empty : rs_head_signed_s;
    BOOST_REQUIRE_EQUAL(rs_head_s, rs_head_signed_s_);

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
    const auto key_id = cache::SignedHead::encode_key_id(pk);
    BOOST_REQUIRE(key_id == ("ed25519=" + inj_b64pk));

    // Add an unexpected header.
    // It should not break signature verification, but it should be removed from its output.
    rs_head_signed.set("X-Foo", "bar");
    // Move a header, keeping the same value.
    // It should not break signature verification.
    auto date = std::string(rs_head_signed[http::field::date]);
    rs_head_signed.erase(http::field::date);
    rs_head_signed.set(http::field::date, date);

    auto vfy_res = cache::SignedHead::verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res);  // successful verification
    BOOST_REQUIRE((*vfy_res)["X-Foo"].empty());
    // TODO: check same headers

    // Add a bad third signature (by altering the second one).
    // It should not break signature verification, but it should be removed from its output.
    auto sig1_copy = std::string(rs_head_signed["X-Ouinet-Sig1"]);
    string sstart(",signature=\"");
    auto spos = sig1_copy.find(sstart);
    BOOST_REQUIRE(spos != string::npos);
    sig1_copy.replace(spos + sstart.length(), 7, "GARBAGE");  // change signature
    rs_head_signed.set("X-Ouinet-Sig2", sig1_copy);

    vfy_res = cache::SignedHead::verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res);  // successful verification
    BOOST_REQUIRE((*vfy_res)["X-Ouinet-Sig2"].empty());

    // Change the key id of the third signature to refer to some other key.
    // It should not break signature verification, and it should be kept in its output.
    auto kpos = sig1_copy.find(inj_b64pk);
    BOOST_REQUIRE(kpos != string::npos);
    sig1_copy.replace(kpos, 7, "GARBAGE");  // change keyId
    rs_head_signed.set("X-Ouinet-Sig2", sig1_copy);

    vfy_res = cache::SignedHead::verify(rs_head_signed, pk);
    BOOST_REQUIRE(vfy_res);  // successful verification
    BOOST_REQUIRE(!(*vfy_res)["X-Ouinet-Sig2"].empty());
    // TODO: check same headers

    // Alter the value of one of the signed headers and verify again.
    // It should break signature verification.
    rs_head_signed.set(http::field::server, "NginX");
    vfy_res = cache::SignedHead::verify(rs_head_signed, pk);
    BOOST_REQUIRE(!vfy_res);  // unsuccessful verification

}

BOOST_DATA_TEST_CASE(test_http_flush_signed, boost::unit_test::data::make(true_false), empty) {
    asio::io_context ctx;
    auto exec = ctx.get_executor();
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(exec);

        asio::ip::tcp::socket
            origin_w(exec), origin_r(exec),
            signed_w(exec), signed_r(exec),
            tested_w(exec), tested_r(exec);
        tie(origin_w, origin_r) = util::connected_pair(yield);
        tie(signed_w, signed_r) = util::connected_pair(yield);
        tie(tested_w, tested_r) = util::connected_pair(yield);

        // Send raw origin response.
        yield.spawn([&origin_w, empty, lock = wc.lock()] (auto y) {
            const auto& rs_head_s_ = empty ? rs_head_s_empty : rs_head_s;
            unwrap(asio::async_write( origin_w
                                    , asio::const_buffer(rs_head_s_.data(), rs_head_s_.size())
                                    , y));
            const auto& rs_body_ = empty ? rs_body_empty : rs_body;
            unwrap(asio::async_write( origin_w
                                    , asio::const_buffer(rs_body_.data(), rs_body_.size())
                                    , y));
            origin_w.close();
        });

        // Sign origin response.
        yield.spawn([origin_r = std::move(origin_r), &signed_w, lock = wc.lock()] (auto y) mutable {
            auto req_h = get_request_header();
            auto sk = get_private_key();
            Session::reader_uptr origin_rvr = make_unique<cache::SigningReader>
                (std::move(origin_r), std::move(req_h), inj_id, inj_ts, sk);
            auto origin_rs = unwrap(Session::create(std::move(origin_rvr), false, y));
            unwrap(origin_rs.flush_response(signed_w, y));
            signed_w.close();
        });

        // Test signed output.
        yield.spawn([signed_r = std::move(signed_r), &tested_w, empty, lock = wc.lock()](auto y) mutable {
            size_t xidx = 0;
            http_response::Reader rr(std::move(signed_r));
            while (true) {
                auto opt_part = unwrap(rr.async_read_part(y));
                if (!opt_part) break;
                if (auto inh = opt_part->as_head()) {
                    auto hbsh = (*inh)[http_::response_block_signatures_hdr];
                    BOOST_REQUIRE(!hbsh.empty());
                    auto hbs = cache::SignedHead::BlockSigs::parse(hbsh);
                    BOOST_REQUIRE(hbs);
                    // Test data block signatures are split according to this size.
                    BOOST_CHECK_EQUAL(hbs->size, inj_bs);
                } else if (auto ch = opt_part->as_chunk_hdr()) {
                    if (!ch->exts.empty()) {
                        if (empty) {
                            BOOST_REQUIRE(xidx < rs_block_sig_cx_empty.size());
                            BOOST_CHECK_EQUAL(ch->exts, rs_block_sig_cx_empty[xidx++]);
                        } else {
                            BOOST_REQUIRE(xidx < rs_block_sig_cx.size());
                            BOOST_CHECK_EQUAL(ch->exts, rs_block_sig_cx[xidx++]);
                        }
                    }
                }
                unwrap(opt_part->async_write(tested_w, y));
            }
            if (empty)
                BOOST_CHECK_EQUAL(xidx, rs_block_sig_cx_empty.size());
            else
                BOOST_CHECK_EQUAL(xidx, rs_block_sig_cx.size());
            tested_w.close();
        });

        // Black hole.
        yield.spawn([&tested_r, lock = wc.lock()] (auto y) {
            read_until_end(tested_r, y);
        });

        wc.wait(yield);
    });
}

BOOST_DATA_TEST_CASE(test_http_flush_verified, boost::unit_test::data::make(true_false), empty) {
    asio::io_context ctx;
    auto exec = ctx.get_executor();
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(exec);

        asio::ip::tcp::socket
            origin_w(exec), origin_r(exec),
            signed_w(exec), signed_r(exec),
            hashed_w(exec), hashed_r(exec),
            tested_w(exec), tested_r(exec);
        tie(origin_w, origin_r) = util::connected_pair(yield);
        tie(signed_w, signed_r) = util::connected_pair(yield);
        tie(hashed_w, hashed_r) = util::connected_pair(yield);
        tie(tested_w, tested_r) = util::connected_pair(yield);

        // Send raw origin response.
        yield.spawn([&origin_w, empty, lock = wc.lock()] (auto y) {
            const auto& rs_head_s_ = empty ? rs_head_s_empty : rs_head_s;
            unwrap(asio::async_write( origin_w
                                    , asio::const_buffer(rs_head_s_.data(), rs_head_s_.size())
                                    , y));
            const auto& rs_body_ = empty ? rs_body_empty : rs_body;
            unwrap(asio::async_write( origin_w
                                    , asio::const_buffer(rs_body_.data(), rs_body_.size())
                                    , y));
            origin_w.close();
        });

        // Sign origin response.
        yield.spawn([origin_r = std::move(origin_r), &signed_w, lock = wc.lock()] (auto y) mutable {
            auto req_h = get_request_header();
            auto sk = get_private_key();
            Session::reader_uptr origin_rvr = make_unique<cache::SigningReader>
                (std::move(origin_r), std::move(req_h), inj_id, inj_ts, sk);
            auto origin_rs = unwrap(Session::create(std::move(origin_rvr), false, y));
            unwrap(origin_rs.flush_response(signed_w, y));
            signed_w.close();
        });

        // Verify signed output.
        yield.spawn([ signed_r = std::move(signed_r), &hashed_w
                         , lock = wc.lock()](auto y) mutable {
            auto pk = get_public_key();
            Session::reader_uptr signed_rvr = make_unique<cache::VerifyingReader>
                (std::move(signed_r), pk);
            auto signed_rs = unwrap(Session::create(std::move(signed_rvr), false, y));
            unwrap(signed_rs.flush_response(hashed_w, y));
            hashed_w.close();
        });

        // Check generation of chained hashes.
        yield.spawn([ hashed_r = std::move(hashed_r), &tested_w, empty
                         , lock = wc.lock()](auto y) mutable {
            size_t xidx = 0;
            http_response::Reader rr(std::move(hashed_r));
            while (true) {
                auto opt_part = unwrap(rr.async_read_part(y));
                if (!opt_part) break;
                if (auto ch = opt_part->as_chunk_hdr()) {
                    if (!ch->exts.empty()) {
                        if (empty) {
                            BOOST_REQUIRE(xidx < rs_block_hash_cx_empty.size());
                            BOOST_CHECK(ch->exts.find(rs_block_hash_cx_empty[xidx++]) != string::npos);
                        } else {
                            BOOST_REQUIRE(xidx < rs_block_hash_cx.size());
                            BOOST_CHECK(ch->exts.find(rs_block_hash_cx[xidx++]) != string::npos);
                        }
                    }
                }
                unwrap(opt_part->async_write(tested_w, y));
            }
            if (empty)
                BOOST_CHECK_EQUAL(xidx, rs_block_hash_cx_empty.size());
            else
                BOOST_CHECK_EQUAL(xidx, rs_block_hash_cx.size());
            tested_w.close();
        });

        // Black hole.
        yield.spawn([&tested_r, lock = wc.lock()] (auto y) {
            read_until_end(tested_r, y);
        });

        wc.wait(yield);
    });
}

BOOST_AUTO_TEST_CASE(test_http_flush_forged) {
    asio::io_context ctx;
    auto exec = ctx.get_executor();
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(exec);

        asio::ip::tcp::socket
            origin_w(exec), origin_r(exec),
            signed_w(exec), signed_r(exec),
            forged_w(exec), forged_r(exec),
            tested_w(exec), tested_r(exec);
        tie(origin_w, origin_r) = util::connected_pair(yield);
        tie(signed_w, signed_r) = util::connected_pair(yield);
        tie(forged_w, forged_r) = util::connected_pair(yield);
        tie(tested_w, tested_r) = util::connected_pair(yield);

        // Send raw origin response.
        yield.spawn([&origin_w, lock = wc.lock()] (auto y) {
            unwrap(asio::async_write( origin_w
                                    , asio::const_buffer(rs_head_s.data(), rs_head_s.size())
                                    , y));
            unwrap(asio::async_write( origin_w
                                    , asio::const_buffer(rs_body.data(), rs_body.size())
                                    , y));
            origin_w.close();
        });

        // Sign origin response.
        yield.spawn([ origin_r = std::move(origin_r), &signed_w
                    , lock = wc.lock()] (auto y) mutable {
            auto req_h = get_request_header();
            auto sk = get_private_key();
            Session::reader_uptr origin_rvr = make_unique<cache::SigningReader>
                (std::move(origin_r), std::move(req_h), inj_id, inj_ts, sk);

            auto origin_rs = unwrap(Session::create(std::move(origin_rvr), false, y));

            unwrap(origin_rs.flush_response(signed_w, y));
            signed_w.close();
        });

        // Forge (alter) signed output.
        yield.spawn([&signed_r, &forged_w, lock = wc.lock()] (auto y) {
            char d[2048];
            asio::mutable_buffer b(d, sizeof(d));
            auto bsv = util::bytes::to_string_view(b);

            while (true) {
                auto read = signed_r.async_read_some(b, y);
                if (!read) {
                    BOOST_REQUIRE(read.error() == asio::error::eof);
                    break;
                }

                // Alter forwarded content somewhere in the second data block.
                auto rci = bsv.find(rs_block1_tail);
                if (rci != string::npos)
                    d[rci] = rs_block1_tail[0] + 1;

                auto write = asio::async_write(forged_w, asio::buffer(b, *read), y);
                if (!write) {
                    BOOST_REQUIRE(write.error() == asio::error::eof);
                    break;
                }
            }

            signed_r.close();
            forged_w.close();
        });

        // Verify forged output.
        yield.spawn([ forged_r = std::move(forged_r), &tested_w
                    , lock = wc.lock()](auto y) mutable {
            auto pk = get_public_key();
            Session::reader_uptr forged_rvr = make_unique<cache::VerifyingReader>
                (std::move(forged_r), pk);
            auto forged_rs = unwrap(Session::create(std::move(forged_rvr), false, y));
            auto r = forged_rs.flush_response(tested_w, y);
            BOOST_REQUIRE(!r);
            BOOST_REQUIRE_EQUAL(r.error().value(), sys::errc::bad_message);
            tested_w.close();
        });

        // Black hole.
        yield.spawn([&tested_r, lock = wc.lock()] (auto y) {
            read_until_end(tested_r, y);
        });

        wc.wait(yield);
    });
}

// Send the signed response with all signature headers at the initial head
// (i.e. no trailers).
BOOST_AUTO_TEST_CASE(test_http_flush_verified_no_trailer) {
    asio::io_context ctx;
    auto exec = ctx.get_executor();
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(exec);

        asio::ip::tcp::socket
            signed_w(exec), signed_r(exec),
            hashed_w(exec), hashed_r(exec),
            tested_w(exec), tested_r(exec);
        tie(signed_w, signed_r) = util::connected_pair(yield);
        tie(hashed_w, hashed_r) = util::connected_pair(yield);
        tie(tested_w, tested_r) = util::connected_pair(yield);

        // Send signed response.
        yield.spawn([&signed_w, lock = wc.lock()] (auto y) {
            // Head (raw).  With trailers as normal headers.
            auto trh_start = rs_head_signed_s.find("Trailer:");
            BOOST_REQUIRE(trh_start != string::npos);
            auto trh_end = rs_head_signed_s.find("\r\n", trh_start);
            BOOST_REQUIRE(trh_start != string::npos);
            auto rs_head = rs_head_signed_s;
            rs_head.erase(trh_start, trh_end - trh_start + 2);  // remove "Trailer: ...\r\n"
            unwrap(asio::async_write( signed_w
                                    , asio::const_buffer(rs_head.data(), rs_head.size())
                                    , y));

            // Chunk headers and bodies (one chunk per block).
            unsigned bi;
            for (bi = 0; bi < rs_block_data.size(); ++bi) {
                auto cbd = util::bytes::to_vector<uint8_t>(rs_block_data[bi]);
                auto ch = http_response::ChunkHdr(cbd.size(), rs_chunk_ext[bi]);
                unwrap(ch.async_write(signed_w, y));
                auto cb = http_response::ChunkBody(std::move(cbd), 0);
                unwrap(cb.async_write(signed_w, y));
            }

            // Last chunk and trailer (raw).
            auto chZ = http_response::ChunkHdr(0, rs_chunk_ext[bi]);
            unwrap(chZ.async_write(signed_w, y));
            http_response::Trailer tr;  // empty, everything was in head
            unwrap(tr.async_write(signed_w, y));

            signed_w.close();
        });

        // Verify signed output.
        yield.spawn([ signed_r = std::move(signed_r), &hashed_w
                    , lock = wc.lock()](auto y) mutable {
            auto pk = get_public_key();
            Session::reader_uptr signed_rvr = make_unique<cache::VerifyingReader>
                (std::move(signed_r), pk);
            auto signed_rs = unwrap(Session::create(std::move(signed_rvr), false, y));
            unwrap(signed_rs.flush_response(hashed_w, y));
            hashed_w.close();
        });

        // Check generation of chained hashes.
        yield.spawn([hashed_r = std::move(hashed_r), &tested_w, lock = wc.lock()](auto y) mutable {
            size_t xidx = 0;
            http_response::Reader rr(std::move(hashed_r));
            while (true) {
                auto opt_part = unwrap(rr.async_read_part(y));
                if (!opt_part) break;
                if (auto ch = opt_part->as_chunk_hdr()) {
                    if (!ch->exts.empty()) {
                        BOOST_REQUIRE(xidx < rs_block_hash_cx.size());
                        BOOST_CHECK(ch->exts.find(rs_block_hash_cx[xidx++]) != string::npos);
                    }
                }
                unwrap(opt_part->async_write(tested_w, y));
            }
            BOOST_CHECK_EQUAL(xidx, rs_block_hash_cx.size());
            tested_w.close();
        });

        // Black hole.
        yield.spawn([&tested_r, lock = wc.lock()] (auto y) {
            read_until_end(tested_r, y);
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
        , _get_digest_fields(rs_body.size(), rs_body_b64digest)
        , _get_signature_field(true, rs_body.size(), rs_body_b64digest)
        , "X-Ouinet-HTTP-Status: 200\r\n"
        , "Content-Range: bytes ", first, '-', last, "/", rs_body.size() ,"\r\n"
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
// These should work as well,
// but the `ouipsig` chunk extension is not yet implemented.
// TODO: implement `ouipsig`
/*
    {1, 2},  // two last blocks
    {2, 2},  // just last block
*/
};

BOOST_DATA_TEST_CASE( test_http_flush_verified_partial
                    , boost::unit_test::data::make(block_ranges), firstb_lastb) {
    asio::io_context ctx;
    auto exec = ctx.get_executor();
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(exec);

        asio::ip::tcp::socket
            signed_w(exec), signed_r(exec),
            tested_w(exec), tested_r(exec);
        tie(signed_w, signed_r) = util::connected_pair(yield);
        tie(tested_w, tested_r) = util::connected_pair(yield);

        unsigned first_block, last_block;
        tie(first_block, last_block) = firstb_lastb;

        // Send partial response.
        yield.spawn([&signed_w , first_block, last_block , lock = wc.lock()] (auto y) {
            // Head (raw).
            auto rsp_head = rs_head_partial(first_block, last_block);
            unwrap(asio::async_write( signed_w
                                    , asio::const_buffer(rsp_head.data(), rsp_head.size())
                                    , y));

            // Chunk headers and bodies (one chunk per block).
            // We start on the first block of the partial range.
            bool first_chunk = true;
            unsigned bi;
            for (bi = first_block; bi <= last_block; ++bi, first_chunk=false) {
                auto cbd = util::bytes::to_vector<uint8_t>(rs_block_data[bi]);
                auto ch = http_response::ChunkHdr( cbd.size()
                                                 , first_chunk ? "" : rs_chunk_ext_partial[bi]);
                unwrap(ch.async_write(signed_w, y));
                auto cb = http_response::ChunkBody(std::move(cbd), 0);
                unwrap(cb.async_write(signed_w, y));
            }

            // Last chunk and empty trailer.
            auto chZ = http_response::ChunkHdr(0, rs_chunk_ext_partial[bi]);
            unwrap(chZ.async_write(signed_w, y));
            auto tr = http_response::Trailer();
            unwrap(tr.async_write(signed_w, y));

            signed_w.close();
        });

        // Test the loaded response.
        yield.spawn([ signed_r = std::move(signed_r), &tested_w
                    , lock = wc.lock()] (auto y) mutable {
            Cancel cancel;
            auto pk = get_public_key();
            Session::reader_uptr signed_rvr = make_unique<cache::VerifyingReader>
                ( std::move(signed_r), pk
                , cache::VerifyingReader::status_set{http::status::partial_content});
            auto signed_rs = unwrap(Session::create(std::move(signed_rvr), false, y));
            unwrap(signed_rs.flush_response(tested_w, y));
            tested_w.close();
        });

        // Black hole.
        yield.spawn([&tested_r, lock = wc.lock()] (auto y) {
            read_until_end(tested_r, y);
        });

        wc.wait(yield);
    });
}
