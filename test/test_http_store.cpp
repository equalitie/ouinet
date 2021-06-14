#define BOOST_TEST_MODULE http_store
#include <boost/test/data/test_case.hpp>
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <sstream>
#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/write.hpp>
#include <boost/filesystem.hpp>

#include <cache/http_sign.h>
#include <cache/http_store.h>
#include <cache/chain_hasher.h>
#include <defer.h>
#include <response_part.h>
#include <session.h>
#include <util/bytes.h>
#include <util/file_io.h>
#include <util/str.h>

#include <namespaces.h>
#include "connected_pair.h"

// For checks to be able to report errors.
namespace ouinet { namespace http_response {
    std::ostream& operator<<(std::ostream& os, const ChunkHdr& hdr) {
        return os << "ChunkHdr(" << hdr.size << ", \"" << hdr.exts << "\")";
    }

    std::ostream& operator<<(std::ostream& os, const Trailer& trailer) {
        return os << static_cast<Trailer::Base>(trailer);
    }
}} // namespace ouinet::http_response

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

BOOST_AUTO_TEST_SUITE(ouinet_http_store)

using namespace std;
using namespace ouinet;

// This signed response used below comes from `test-http-sign`.
// TODO: Have signatures and hashes computed at runtime to
// avoid having to manually update this data every time there are
// signing protocol changes.

static const string _rs_status_origin =
    "HTTP/1.1 200 OK\r\n";
static const string _rs_fields_origin = (
    "Date: Mon, 15 Jan 2018 20:31:50 GMT\r\n"
    "Server: Apache1\r\n"
    "Server: Apache2\r\n"
    "Content-Type: text/html\r\n"
    "Content-Disposition: inline; filename=\"foo.html\"\r\n"
);

static const string _rs_head_origin =
    ( _rs_status_origin
    + _rs_fields_origin);

static const string _rs_head_injection = (
    "X-Ouinet-Version: 6\r\n"
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
    "signature=\"qs/iL8KDytc22DqSBwhkEf/RoguMcQKcorrwviQx9Ck0SBf0A4Hby+dMpHDk9mjNYYnLCw4G9vPN637hG3lkAQ==\"\r\n"
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
    "signature=\"4+POBKdNljxUKHKD+NCP34aS6j0QhI4EWmqiN3aopoWtDiMwgmeiR1hO44QhWFwWdNmNkVJs+LVuEUN892mFDg==\"\r\n"
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

static const array<util::SHA512::digest_type, 3> rs_block_dhash_raw{
    util::SHA512::digest(rs_block_data[0]),
    util::SHA512::digest(rs_block_data[1]),
    util::SHA512::digest(rs_block_data[2])
};

static const array<string, 3> rs_block_dhash{
    util::base64_encode(rs_block_dhash_raw[0]),
    util::base64_encode(rs_block_dhash_raw[1]),
    util::base64_encode(rs_block_dhash_raw[2])
};

static const array<string, 3> rs_block_sig{
    "r2OtBbBVBXT2b8Ch/eFfQt1eDoG8eMs/JQxnjzNPquF80WcUNwQQktsu0mF0+bwc3akKdYdBDeORNLhRjrxVBA==",
    "LfRN72Vv5QMNd6sn6HOWbfcoN6DA9kdjTXEfJvmgViZQZT5hlZXQpCOULyBreeZv3sd7j5FJzgu3CCUoBXOCCA==",
    "oZ3hLELDPOK4y2b0Yd6ezoXaF37PqBXt/WX7YJAzfS4au/QewCQxMlds8qtNWjOrP9Gzyde3jjFn647srWI7DA==",
};

// As they appear in signature files.
static util::SHA512::digest_type rs_block_chash_raw(size_t i) {
    using Signature = util::Ed25519PublicKey::sig_array_t;

    if (i == 0) return util::SHA512::zero_digest();

    cache::ChainHasher chain_hasher;
    cache::ChainHash chain_hash;

    for (size_t j = 0; j < i; ++j) {
        chain_hash = chain_hasher.calculate_block(
                rs_block_data[j].size(),
                rs_block_dhash_raw[j],
                *util::base64_decode<Signature>(rs_block_sig[j]));
    }

    return chain_hash.chain_digest;
}

static string rs_block_chash(size_t i) {
    return util::base64_encode(rs_block_chash_raw(i));
}

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

void store_response( const fs::path& tmpdir, bool complete
                   , asio::io_context& ctx, asio::yield_context yield) {
    asio::ip::tcp::socket
        signed_w(ctx), signed_r(ctx);
    tie(signed_w, signed_r) = util::connected_pair(ctx, yield);

    WaitCondition wc(ctx);

    // Send signed response.
    asio::spawn(ctx, [&signed_w, complete, lock = wc.lock()] (auto y) {
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

        if (!complete) {  // no last chunk nor trailer
            // Last block signature should be missing
            // and its data should not be sent when reading
            // even if available on disk.
            signed_w.close();
            return;
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
    asio::spawn(ctx, [ signed_r = std::move(signed_r), &tmpdir, complete
                     , &ctx, lock = wc.lock()] (auto y) mutable {
        Cancel c;
        sys::error_code e;
        http_response::Reader signed_rr(std::move(signed_r));
        cache::http_store(signed_rr, tmpdir, ctx.get_executor(), c, y[e]);
        BOOST_CHECK(!complete || !e);
    });

    wc.wait(yield);
}

void store_response_external( const fs::path& tmpdir, const fs::path& tmpcdir
                            , asio::io_context& ctx, asio::yield_context yield) {
    store_response(tmpdir, true, ctx, yield);

    // Move body to external file, point `body-path` to it.
    auto crpath = fs::path("foo/bar/data.dat");
    {
        auto cpath = tmpcdir / crpath;
        fs::create_directories(cpath.branch_path());
        fs::rename(tmpdir / "body", cpath);
    }
    {
        sys::error_code ec;
        auto body_path_f = util::file_io::open_or_create(ctx.get_executor(), tmpdir / "body-path", ec);
        if (ec) return or_throw(yield, ec);
        auto crpath_b = asio::const_buffer(crpath.string().data(), crpath.string().size());
        Cancel cancel;
        util::file_io::write(body_path_f, crpath_b, cancel, yield);
    }
}

// Values for empty body tests.
static const string _ers_head_digest = (
    "X-Ouinet-Data-Size: 0\r\n"
    "Digest: SHA-256=47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=\r\n"
);

static const string ers_trailer =
  ( _ers_head_digest
  + _rs_head_sig1  // would fail sig verification, but ok for test
  + "\r\n");

static const string ers_last_chunk_ext = // dummy value for test
  ";ouisig=\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==\"";

void store_empty_response( const fs::path& tmpdir
                         , asio::io_context& ctx, asio::yield_context yield) {
    asio::ip::tcp::socket
        signed_w(ctx), signed_r(ctx);
    tie(signed_w, signed_r) = util::connected_pair(ctx, yield);

    WaitCondition wc(ctx);

    // Send signed response.
    asio::spawn(ctx, [&signed_w, lock = wc.lock()] (auto y) {
        // Head (raw).
        asio::async_write( signed_w
                         , asio::const_buffer(rs_head.data(), rs_head.size())
                         , y);
        // Last chunk and trailer (raw).
        auto chZ = http_response::ChunkHdr(0, ers_last_chunk_ext);
        chZ.async_write(signed_w, y);
        asio::async_write( signed_w
                         , asio::const_buffer(ers_trailer.data(), ers_trailer.size())
                         , y);

        signed_w.close();
    });

    // Store response.
    asio::spawn(ctx, [ signed_r = std::move(signed_r), &tmpdir
                     , &ctx, lock = wc.lock()] (auto y) mutable {
        Cancel c;
        sys::error_code e;
        http_response::Reader signed_rr(std::move(signed_r));
        cache::http_store(signed_rr, tmpdir, ctx.get_executor(), c, y[e]);
        BOOST_CHECK_EQUAL(e.message(), "Success");
    });

    wc.wait(yield);
}

void store_response_head( const fs::path& tmpdir, const string& head_s
                        , asio::io_context& ctx, asio::yield_context yield) {
    asio::ip::tcp::socket
        signed_w(ctx), signed_r(ctx);
    tie(signed_w, signed_r) = util::connected_pair(ctx, yield);

    WaitCondition wc(ctx);

    // Send signed response.
    asio::spawn(ctx, [&signed_w, &head_s, lock = wc.lock()] (auto y) {
        // Head (raw).
        asio::async_write( signed_w
                         , asio::const_buffer(head_s.data(), head_s.size())
                         , y);
        signed_w.close();
    });

    // Store response.
    asio::spawn(ctx, [ signed_r = std::move(signed_r), &tmpdir
                     , &ctx, lock = wc.lock()] (auto y) mutable {
        Cancel c;
        sys::error_code e;
        http_response::Reader signed_rr(std::move(signed_r));
        cache::http_store(signed_rr, tmpdir, ctx.get_executor(), c, y[e]);
    });

    wc.wait(yield);
}

static const string rs_head_incomplete =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_sig0
    + "\r\n");

static const string rs_body_incomplete =
    ( rs_block_data[0]
    + rs_block_data[1]);

static const string rs_head_complete =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_digest
    + _rs_head_sig1
    + "\r\n");

static const string rs_body_complete =
    ( rs_block_data[0]
    + rs_block_data[1]
    + rs_block_data[2]);

static string rs_sigs(bool complete) {
    stringstream ss;
    // Last signature missing when incomplete.
    auto last_b = complete ? rs_block_data.size() : rs_block_data.size() - 1;
    for (size_t b = 0; b < last_b; ++b)
        ss << hex << setfill('0') << setw(16) // 16 is length of hex 2^64-1
           << (b * http_::response_data_block)
           << ' ' << rs_block_sig[b] << ' ' << rs_block_dhash[b] << ' ' << rs_block_chash(b)
           << endl;
    return ss.str();
}

static const bool true_false[] = {true, false};

BOOST_DATA_TEST_CASE(test_write_response, boost::unit_test::data::make(true_false), complete) {
    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });
    fs::create_directory(tmpdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        store_response(tmpdir, complete, ctx, yield);

        auto read_file = [&] (auto fname, auto c, auto y) -> string {
            sys::error_code e;
            auto f = util::file_io::open_readonly(ctx.get_executor(), tmpdir / fname, e);
            if (e) return or_throw(y, e, "");

            size_t fsz = util::file_io::file_size(f, e);
            if (e) return or_throw(y, e, "");

            std::string fdata(fsz, '\0');
            util::file_io::read(f, asio::buffer(fdata), c, y[e]);
            return_or_throw_on_error(y, c, e, "");

            return fdata;
        };

        Cancel cancel;
        sys::error_code ec;

        auto head = read_file("head", cancel, yield[ec]);
        BOOST_CHECK_EQUAL(ec.message(), "Success");
        BOOST_CHECK_EQUAL(head, complete ? rs_head_complete :  rs_head_incomplete);

        auto body = read_file("body", cancel, yield[ec]);
        BOOST_CHECK_EQUAL(ec.message(), "Success");
        BOOST_CHECK_EQUAL(body, rs_body_complete);

        auto sigs = read_file("sigs", cancel, yield[ec]);
        BOOST_CHECK_EQUAL(ec.message(), "Success");
        BOOST_CHECK_EQUAL(sigs, rs_sigs(complete));
    });
}

BOOST_AUTO_TEST_CASE(test_read_response_missing) {
    auto tmpdir = fs::unique_path();
    asio::io_context ctx;
    sys::error_code ec;
    auto store_rr = cache::http_store_reader(tmpdir, ctx.get_executor(), ec);
    BOOST_CHECK(!store_rr);
    BOOST_CHECK_EQUAL(ec, sys::errc::no_such_file_or_directory);
}


static const string rrs_head_incomplete =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_sig0
    + "Transfer-Encoding: chunked\r\n"
    + "\r\n");

static const string rrs_head_complete =
    ( _rs_head_origin
    + _rs_head_injection
    + _rs_head_digest
    + _rs_head_sig1
    + "Transfer-Encoding: chunked\r\n"
    + "\r\n");

// TODO: implement `ouipsig`
static const array<string, 4> rrs_chunk_ext{
    "",
    ";ouisig=\"" + rs_block_sig[0] + "\"",
    ";ouisig=\"" + rs_block_sig[1] + "\";ouihash=\"" + rs_block_chash(1) + "\"",
    ";ouisig=\"" + rs_block_sig[2] + "\";ouihash=\"" + rs_block_chash(2) + "\"",
};

// Trailers are merged into the initial head,
// so the loaded trailer is always empty.
static const http_response::Trailer rrs_trailer{};

BOOST_DATA_TEST_CASE(test_read_response, boost::unit_test::data::make(true_false), complete) {
    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });
    fs::create_directory(tmpdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        store_response(tmpdir, complete, ctx, yield);

        asio::ip::tcp::socket
            loaded_w(ctx), loaded_r(ctx);
        tie(loaded_w, loaded_r) = util::connected_pair(ctx, yield);

        WaitCondition wc(ctx);

        // Load response.
        asio::spawn(ctx, [ &loaded_w, &tmpdir, complete
                         , &ctx, lock = wc.lock()] (auto y) {
            Cancel c;
            sys::error_code e;
            auto store_rr = cache::http_store_reader(tmpdir, ctx.get_executor(), e);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(store_rr);
            auto store_s = Session::create(std::move(store_rr), false, c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            store_s.flush_response(loaded_w, c, y[e]);
            BOOST_CHECK(!complete || !e);
            loaded_w.close();
        });

        // Check parts of the loaded response.
        asio::spawn(ctx, [ loaded_r = std::move(loaded_r), complete
                         , lock = wc.lock()] (auto y) mutable {
            Cancel c;
            sys::error_code e;
            http_response::Reader loaded_rr(std::move(loaded_r));

            // Head.
            auto part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_head());
            BOOST_REQUIRE_EQUAL( util::str(*(part->as_head()))
                               , complete ? rrs_head_complete : rrs_head_incomplete);

            // Chunk headers and bodies (one chunk per block).
            unsigned bi;
            for (bi = 0; bi < rs_block_data.size(); ++bi) {
                part = loaded_rr.async_read_part(c, y[e]);
                BOOST_CHECK_EQUAL(e.message(), "Success");
                BOOST_REQUIRE(part);
                BOOST_REQUIRE(part->is_chunk_hdr());
                BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                                   , http_response::ChunkHdr( rs_block_data[bi].size()
                                                            , rrs_chunk_ext[bi]));

                // For the incomplete test, the last block signature should be missing,
                // so we will not get its data.
                if (!complete && bi == rs_block_data.size() - 1) {
                    part = loaded_rr.async_read_part(c, y[e]);
                    BOOST_REQUIRE(!part);
                    break;
                }
                std::vector<uint8_t> bd;  // accumulate data here
                for (bool done = false; !done; ) {
                    part = loaded_rr.async_read_part(c, y[e]);
                    BOOST_CHECK_EQUAL(e.message(), "Success");
                    BOOST_REQUIRE(part);
                    BOOST_REQUIRE(part->is_chunk_body());
                    auto& d = *(part->as_chunk_body());
                    bd.insert(bd.end(), d.cbegin(), d.cend());
                    done = (d.remain == 0);
                }
                BOOST_REQUIRE_EQUAL( util::bytes::to_string(bd)
                                   , rs_block_data[bi]);
            }

            if (!complete) return;

            // Last chunk header.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_chunk_hdr());
            BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                               , http_response::ChunkHdr( 0
                                                        , rrs_chunk_ext[bi]));

            // Trailer.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_trailer());
            BOOST_CHECK_EQUAL(*(part->as_trailer()), rrs_trailer);
        });

        wc.wait(yield);
    });
}

BOOST_AUTO_TEST_CASE(test_read_response_external) {
    auto tmpdir = fs::unique_path();
    auto tmpcdir = fs::unique_path();
    auto rmdirs = defer([&tmpdir, &tmpcdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec = {});
        fs::remove_all(tmpcdir, ec = {});
    });
    fs::create_directory(tmpdir);
    fs::create_directory(tmpcdir);
    tmpcdir = fs::canonical(tmpcdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        store_response_external(tmpdir, tmpcdir, ctx, yield);

        asio::ip::tcp::socket
            loaded_w(ctx), loaded_r(ctx);
        tie(loaded_w, loaded_r) = util::connected_pair(ctx, yield);

        WaitCondition wc(ctx);

        // Load response.
        asio::spawn(ctx, [ &loaded_w, &tmpdir, &tmpcdir
                         , &ctx, lock = wc.lock()] (auto y) {
            Cancel c;
            sys::error_code e;
            auto store_rr = cache::http_store_reader(tmpdir, tmpcdir, ctx.get_executor(), e);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(store_rr);
            auto store_s = Session::create(std::move(store_rr), false, c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            store_s.flush_response(loaded_w, c, y[e]);
            BOOST_CHECK(!e);
            loaded_w.close();
        });

        // Check parts of the loaded response.
        asio::spawn(ctx, [ loaded_r = std::move(loaded_r)
                         , lock = wc.lock()] (auto y) mutable {
            Cancel c;
            sys::error_code e;
            http_response::Reader loaded_rr(std::move(loaded_r));

            // Head.
            auto part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_head());
            BOOST_REQUIRE_EQUAL( util::str(*(part->as_head()))
                               , rrs_head_complete);

            // Chunk headers and bodies (one chunk per block).
            unsigned bi;
            for (bi = 0; bi < rs_block_data.size(); ++bi) {
                part = loaded_rr.async_read_part(c, y[e]);
                BOOST_CHECK_EQUAL(e.message(), "Success");
                BOOST_REQUIRE(part);
                BOOST_REQUIRE(part->is_chunk_hdr());
                BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                                   , http_response::ChunkHdr( rs_block_data[bi].size()
                                                            , rrs_chunk_ext[bi]));

                std::vector<uint8_t> bd;  // accumulate data here
                for (bool done = false; !done; ) {
                    part = loaded_rr.async_read_part(c, y[e]);
                    BOOST_CHECK_EQUAL(e.message(), "Success");
                    BOOST_REQUIRE(part);
                    BOOST_REQUIRE(part->is_chunk_body());
                    auto& d = *(part->as_chunk_body());
                    bd.insert(bd.end(), d.cbegin(), d.cend());
                    done = (d.remain == 0);
                }
                BOOST_REQUIRE_EQUAL( util::bytes::to_string(bd)
                                   , rs_block_data[bi]);
            }

            // Last chunk header.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_chunk_hdr());
            BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                               , http_response::ChunkHdr( 0
                                                        , rrs_chunk_ext[bi]));

            // Trailer.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_trailer());
            BOOST_CHECK_EQUAL(*(part->as_trailer()), rrs_trailer);
        });

        wc.wait(yield);
    });
}

// Values for empty body tests.
static const string errs_head_complete =
    ( _rs_head_origin
    + _rs_head_injection
    + _ers_head_digest
    + _rs_head_sig1  // would fail sig verification, but ok for test
    + "Transfer-Encoding: chunked\r\n"
    + "\r\n");

static const string errs_last_chunk_ext = ers_last_chunk_ext;  // no `ouihash` here

BOOST_AUTO_TEST_CASE(test_read_empty_response) {
    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });
    fs::create_directory(tmpdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        store_empty_response(tmpdir, ctx, yield);

        asio::ip::tcp::socket
            loaded_w(ctx), loaded_r(ctx);
        tie(loaded_w, loaded_r) = util::connected_pair(ctx, yield);

        WaitCondition wc(ctx);

        // Load response.
        asio::spawn(ctx, [ &loaded_w, &tmpdir
                         , &ctx, lock = wc.lock()] (auto y) {
            Cancel c;
            sys::error_code e;
            auto store_rr = cache::http_store_reader(tmpdir, ctx.get_executor(), e);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(store_rr);
            auto store_s = Session::create(std::move(store_rr), false, c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            store_s.flush_response(loaded_w, c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            loaded_w.close();
        });

        // Check parts of the loaded response.
        asio::spawn(ctx, [ loaded_r = std::move(loaded_r)
                         , lock = wc.lock()] (auto y) mutable {
            Cancel c;
            sys::error_code e;
            http_response::Reader loaded_rr(std::move(loaded_r));

            // Head.
            auto part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_head());
            BOOST_REQUIRE_EQUAL( util::str(*(part->as_head()))
                               , errs_head_complete);

            // Last chunk header.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_chunk_hdr());
            BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                               , http_response::ChunkHdr( 0
                                                        , errs_last_chunk_ext));

            // Trailer.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_trailer());
            BOOST_CHECK_EQUAL(*(part->as_trailer()), rrs_trailer);
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
static string rrs_head_partial(unsigned first_block, unsigned last_block) {
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

static const first_last block_ranges[] = {
    {0, 0},  // just first block
    {0, 1},  // two first blocks
    {0, 2},  // all blocks
    {1, 2},  // two last blocks
    {2, 2},  // just last block
};

BOOST_DATA_TEST_CASE( test_read_response_partial
                    , boost::unit_test::data::make(block_ranges), firstb_lastb) {
    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });
    fs::create_directory(tmpdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        store_response(tmpdir, true, ctx, yield);

        asio::ip::tcp::socket
            loaded_w(ctx), loaded_r(ctx);
        tie(loaded_w, loaded_r) = util::connected_pair(ctx, yield);

        WaitCondition wc(ctx);

        // Load partial response:
        // request from middle first block to middle last block.
        // Use first byte *after* middle last block
        // to avoid using an inverted range
        // when first and last blocks match.
        unsigned first_block, last_block;
        tie(first_block, last_block) = firstb_lastb;
        asio::spawn(ctx, [ &loaded_w, &tmpdir
                         , first_block, last_block
                         , &ctx, lock = wc.lock()] (auto y) {
            Cancel c;
            sys::error_code e;
            size_t first = (first_block * http_::response_data_block) + rs_block_data[first_block].size() / 2;
            size_t last = (last_block * http_::response_data_block) + rs_block_data[last_block].size() / 2;
            auto store_rr = cache::http_store_range_reader
                (tmpdir, ctx.get_executor(), first, last, e);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(store_rr);
            auto store_s = Session::create(std::move(store_rr), false, c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            store_s.flush_response(loaded_w, c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            loaded_w.close();
        });

        // Check parts of the loaded response.
        asio::spawn(ctx, [ loaded_r = std::move(loaded_r)
                         , first_block, last_block
                         , lock = wc.lock()] (auto y) mutable {
            Cancel c;
            sys::error_code e;
            http_response::Reader loaded_rr(std::move(loaded_r));

            // Head.
            auto part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_head());
            BOOST_REQUIRE_EQUAL( util::str(*(part->as_head()))
                               , rrs_head_partial(first_block, last_block));

            // Chunk headers and bodies (one chunk per block).
            // We start on the first block of the partial range.
            bool first_chunk = true;
            unsigned bi;
            for (bi = first_block; bi <= last_block; ++bi, first_chunk=false) {
                part = loaded_rr.async_read_part(c, y[e]);
                BOOST_CHECK_EQUAL(e.message(), "Success");
                BOOST_REQUIRE(part);
                BOOST_REQUIRE(part->is_chunk_hdr());
                BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                                   , http_response::ChunkHdr( rs_block_data[bi].size()
                                                            , first_chunk ? "" : rrs_chunk_ext[bi]));

                std::vector<uint8_t> bd;  // accumulate data here
                for (bool done = false; !done; ) {
                    part = loaded_rr.async_read_part(c, y[e]);
                    BOOST_CHECK_EQUAL(e.message(), "Success");
                    BOOST_REQUIRE(part);
                    BOOST_REQUIRE(part->is_chunk_body());
                    auto& d = *(part->as_chunk_body());
                    bd.insert(bd.end(), d.cbegin(), d.cend());
                    done = (d.remain == 0);
                }
                BOOST_REQUIRE_EQUAL( util::bytes::to_string(bd)
                                   , rs_block_data[bi]);
            }

            // Last chunk header.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_chunk_hdr());
            BOOST_REQUIRE_EQUAL( *(part->as_chunk_hdr())
                               , http_response::ChunkHdr( 0
                                                        , rrs_chunk_ext[bi]));

            // Trailer.
            part = loaded_rr.async_read_part(c, y[e]);
            BOOST_CHECK_EQUAL(e.message(), "Success");
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_trailer());
            BOOST_CHECK_EQUAL(*(part->as_trailer()), rrs_trailer);
        });

        wc.wait(yield);
    });
}

BOOST_AUTO_TEST_CASE(test_read_response_partial_off) {
    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });
    fs::create_directory(tmpdir);

    asio::io_context ctx;
    run_spawned(ctx, [&] (auto yield) {
        store_response(tmpdir, true, ctx, yield);

        sys::error_code e;
        auto store_rr = cache::http_store_range_reader
            ( tmpdir, ctx.get_executor()
            , 0, 42'000'000  // off limits
            , e);
        BOOST_CHECK(!e);
        BOOST_CHECK(store_rr);
    });
}

BOOST_DATA_TEST_CASE(test_hash_list, boost::unit_test::data::make(true_false), complete) {
    auto tmpdir = fs::unique_path();
    auto rmdir = defer([&tmpdir] {
        sys::error_code ec;
        fs::remove_all(tmpdir, ec);
    });

    fs::create_directory(tmpdir);

    asio::io_context ctx;
    auto exec = ctx.get_executor();
    Cancel cancel;

    run_spawned(ctx, [&] (auto yield) {
        store_response(tmpdir, complete, ctx, yield);
        cache::HashList hl = cache::http_store_load_hash_list(tmpdir, exec, cancel, yield);
        BOOST_REQUIRE(hl.verify());
    });
}


BOOST_AUTO_TEST_SUITE_END()
