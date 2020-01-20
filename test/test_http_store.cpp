#define BOOST_TEST_MODULE http_store
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/write.hpp>
#include <boost/filesystem.hpp>

#include <cache/http_sign.h>
#include <cache/http_store.h>
#include <defer.h>
#include <response_part.h>
#include <util/bytes.h>
#include <util/file_io.h>
#include <util/connected_pair.h>

#include <namespaces.h>

BOOST_AUTO_TEST_SUITE(ouinet_http_store)

using namespace std;
using namespace ouinet;

// This signed response used below comes from `test-http-sign`.

static const string _rs_head_origin = (
    "HTTP/1.1 200 OK\r\n"
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

static const string rs_head =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_sig0
    + _rs_head_framing
    + "\r\n");

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

static const string rs_trailer =
    ( _rs_head_digest
    + _rs_head_sig1
    + "\r\n");

static const string _rs_block0_head = "0123";
static const string _rs_block0_tail = "4567";
static const string _rs_block1_head = "89AB";
static const string _rs_block1_tail = "CDEF";
static const string _rs_block2 = "abcd";
static const char _rs_block_fill_char = 'x';
static const size_t _rs_block_fill = ( http_::response_data_block
                                     - _rs_block0_head.size()
                                     - _rs_block0_tail.size());

static const array<string, 3> rs_block_data{
    _rs_block0_head + string(_rs_block_fill, _rs_block_fill_char) + _rs_block0_tail,
    _rs_block1_head + string(_rs_block_fill, _rs_block_fill_char) + _rs_block1_tail,
    _rs_block2,
};

static const array<string, 3> rs_block_hash{
    "aERfr5o+kpvR4ZH7xC0mBJ4QjqPUELDzjmzt14WmntxH2p3EQmATZODXMPoFiXaZL6KNI50Ve4WJf/x3ma4ieA==",
    "slwciqMQBddB71VWqpba+MpP9tBiyTE/XFmO5I1oiVJy3iFniKRkksbP78hCEWOM6tH31TGEFWP1loa4pqrLww==",
    "vyUR6T034qN7qDZO5vUILMP9FsJYPys1KIELlGDFCSqSFI7ZowrT3U9ffwsQAZSCLJvKQhT+GhtO0aM2jNnm5A==",
};

static const array<string, 3> rs_block_sig{
    "AwiYuUjLYh/jZz9d0/ev6dpoWqjU/sUWUmGL36/D9tI30oaqFgQGgcbVCyBtl0a7x4saCmxRHC4JW7cYEPWwCw==",
    "c+ZJUJI/kc81q8sLMhwe813Zdc+VPa4DejdVkO5ZhdIPPojbZnRt8OMyFMEiQtHYHXrZIK2+pKj2AO03j70TBA==",
    "m6sz1NpU/8iF6KNN6drY+Yk361GiW0lfa0aaX5TH0GGW/L5GsHyg8ozA0ejm29a+aTjp/qIoI1VrEVj1XG/gDA==",
};

static const array<string, 4> rs_chunk_ext{
    "",
    ";ouisig=\"" + rs_block_sig[0] + "\"",
    ";ouisig=\"" + rs_block_sig[1] + "\"",
    ";ouisig=\"" + rs_block_sig[2] + "\"",
};

template<class F>
static void run_spawned(asio::io_context& ctx, F&& f) {
    asio::spawn(ctx, [&ctx, f = forward<F>(f)] (auto yield) {
            try {
                f(yield);
            }
            catch (const std::exception& e) {
                BOOST_ERROR(string("Test ended with exception: ") + e.what());
            }
        });
    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_write_response) {
    // This test knows about the internals of this particular format.
    BOOST_CHECK_EQUAL(cache::http_store_version, 1);

    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });
    fs::create_directory(tmpdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        WaitCondition wc(ctx);

        asio::ip::tcp::socket
            signed_w(ctx), signed_r(ctx);
        tie(signed_w, signed_r) = util::connected_pair(ctx, yield);

        // Send signed response.
        asio::spawn(ctx, [&signed_w, lock = wc.lock()] (auto y) {
            // Head (raw).
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
            asio::async_write( signed_w
                             , asio::const_buffer(rs_trailer.data(), rs_trailer.size())
                             , y);

            signed_w.close();
        });

        // Store response.
        asio::spawn(ctx, [ signed_r = std::move(signed_r), &tmpdir
                         , &ctx, lock = wc.lock()] (auto y) mutable {
            Cancel c;
            http_response::Reader signed_rr(std::move(signed_r));
            cache::http_store(signed_rr, tmpdir, ctx.get_executor(), c, y);
        });

        wc.wait(yield);
    });

    // TODO: actually check stored data
    sys::error_code ec;
    auto headf = util::file_io::open_readonly(ctx.get_executor(), tmpdir / "head", ec);
    BOOST_CHECK_EQUAL(ec.message(), "Success");
    auto bodyf = util::file_io::open_readonly(ctx.get_executor(), tmpdir / "body", ec);
    BOOST_CHECK_EQUAL(ec.message(), "Success");
    auto sigsf = util::file_io::open_readonly(ctx.get_executor(), tmpdir / "sigs", ec);
    BOOST_CHECK_EQUAL(ec.message(), "Success");
}

BOOST_AUTO_TEST_SUITE_END()
