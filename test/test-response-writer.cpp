#define BOOST_TEST_MODULE response_writer
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <sstream>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>

#include "../src/util/bytes.h"
#include "../src/response_writer.h"
#include "../src/util/wait_condition.h"


using namespace std;
using namespace ouinet;

using tcp = asio::ip::tcp;
using RW = http_response::Writer;

namespace HR = http_response;


// Heads and trailers do not have default comparison operations,
// implement some dummy ones to be able to build.
namespace ouinet { namespace http_response {
    bool operator==(const HR::Head&, const HR::Head&) {
        return false;  // dummy
    }

    bool operator==(const HR::Trailer&, const HR::Trailer&) {
        return false;  // dummy
    }
}} // ouinet namespaces::http_response

// Data written to the returned socket is collected in `outs`,
// and locks in `outwc` are released when an error occurs
// (or the socket is closed).
tcp::socket
stream(stringstream& outs, WaitCondition& outwc, asio::io_service& ios, asio::yield_context yield) {
    tcp::acceptor a(ios, tcp::endpoint(tcp::v4(), 0));
    tcp::socket s1(ios), s2(ios);

    sys::error_code accept_ec;
    sys::error_code connect_ec;

    WaitCondition wc(ios);

    asio::spawn(ios, [&, lock = wc.lock()] (asio::yield_context yield) mutable {
        a.async_accept(s2, yield[accept_ec]);
    });

    s1.async_connect(a.local_endpoint(), yield[connect_ec]);
    wc.wait(yield);

    if (accept_ec)  return or_throw(yield, accept_ec, move(s1));
    if (connect_ec) return or_throw(yield, connect_ec, move(s1));

    asio::spawn(ios, [&outs, done = outwc.lock(), s = move(s2)]
                     (asio::yield_context yield) mutable {
        array<uint8_t, 2048> outd;
        auto outb = asio::buffer(outd);
        size_t len = 0;
        sys::error_code ec;
        do {
            outs << util::bytes::to_string_view(asio::buffer(outb, len));
            len = s.async_read_some(outb, yield[ec]);
        } while (!ec);
    });

    return s1;
}

vector<uint8_t> str_to_vec(boost::string_view s) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
    return {p, p + s.size()};
}


BOOST_AUTO_TEST_SUITE(ouinet_response_writer)

BOOST_AUTO_TEST_CASE(test_http10_no_body) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y_) {
        Cancel c;
        Yield y(ios, y_);

        stringstream outs;
        WaitCondition outwc(ios);
        {
            http::response_header<> rh;
            rh.version(10);
            rh.result(http::status::ok);

            RW rw(stream(outs, outwc, ios, y));
            HR::Part part;

            part = HR::Head(move(rh));
            rw.async_write_part(part, c, y);
        }
        outwc.wait(y);

        const string rsp =
            "HTTP/1.0 200 OK\r\n"
            "\r\n";
        BOOST_REQUIRE_EQUAL(outs.str(), rsp);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http10_body_no_length) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y_) {
        Cancel c;
        Yield y(ios, y_);

        stringstream outs;
        WaitCondition outwc(ios);
        {
            const string rb("abcdef");

            http::response_header<> rh;
            rh.version(10);
            rh.result(http::status::ok);

            RW rw(stream(outs, outwc, ios, y));
            HR::Part part;

            part = HR::Head(move(rh));
            rw.async_write_part(part, c, y);

            part = HR::Body(true, str_to_vec(rb));
            rw.async_write_part(part, c, y);
        }
        outwc.wait(y);

        const string rsp =
            "HTTP/1.0 200 OK\r\n"
            "\r\n"
            "abcdef";
        BOOST_REQUIRE_EQUAL(outs.str(), rsp);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_body) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y_) {
        Cancel c;
        Yield y(ios, y_);

        stringstream outs;
        WaitCondition outwc(ios);
        {
            const string rb("0123456789");

            http::response_header<> rh;
            rh.version(11);
            rh.result(http::status::ok);
            rh.set(http::field::date, "Mon, 27 Jul 2019 12:30:20 GMT");
            rh.set(http::field::content_type, "text/html");
            rh.set(http::field::content_length, rb.size());

            RW rw(stream(outs, outwc, ios, y));
            HR::Part part;

            part = HR::Head(move(rh));
            rw.async_write_part(part, c, y);

            part = HR::Body(true, str_to_vec(rb));
            rw.async_write_part(part, c, y);
        }
        outwc.wait(y);

        const string rsp =
            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2019 12:30:20 GMT\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 10\r\n"
            "\r\n"
            "0123456789";
        BOOST_REQUIRE_EQUAL(outs.str(), rsp);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_chunk) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y_) {
        Cancel c;
        Yield y(ios, y_);

        stringstream outs;
        WaitCondition outwc(ios);
        {
            http::response_header<> rh;
            rh.version(11);
            rh.result(http::status::ok);
            rh.set(http::field::date, "Mon, 27 Jul 2019 12:30:20 GMT");
            rh.set(http::field::content_type, "text/html");
            rh.set(http::field::transfer_encoding, "chunked");

            RW rw(stream(outs, outwc, ios, y));
            HR::Part part;

            part = HR::Head(move(rh));
            rw.async_write_part(part, c, y);

            part = HR::ChunkHdr(4, "");
            rw.async_write_part(part, c, y);

            part = HR::ChunkBody(str_to_vec("12"), 2);
            rw.async_write_part(part, c, y);

            part = HR::ChunkBody(str_to_vec("34"), 0);
            rw.async_write_part(part, c, y);

            part = HR::ChunkHdr(0, "");
            rw.async_write_part(part, c, y);

            part = HR::Trailer();
            rw.async_write_part(part, c, y);
        }
        outwc.wait(y);

        const string rsp =
            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2019 12:30:20 GMT\r\n"
            "Content-Type: text/html\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4\r\n"
            "1234\r\n"
            "0\r\n"
            "\r\n";
        BOOST_REQUIRE_EQUAL(outs.str(), rsp);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_trailer) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y_) {
        Cancel c;
        Yield y(ios, y_);

        stringstream outs;
        WaitCondition outwc(ios);
        {
            http::response_header<> rh;
            rh.version(11);
            rh.result(http::status::ok);
            rh.set(http::field::date, "Mon, 27 Jul 2019 12:30:20 GMT");
            rh.set(http::field::content_type, "text/html");
            rh.set(http::field::transfer_encoding, "chunked");
            rh.set(http::field::trailer, "Hash");

            RW rw(stream(outs, outwc, ios, y));
            HR::Part part;

            part = HR::Head(move(rh));
            rw.async_write_part(part, c, y);

            part = HR::ChunkHdr(4, "");
            rw.async_write_part(part, c, y);

            part = HR::ChunkBody(str_to_vec("12"), 2);
            rw.async_write_part(part, c, y);

            part = HR::ChunkBody(str_to_vec("34"), 0);
            rw.async_write_part(part, c, y);

            part = HR::ChunkHdr(0, "");
            rw.async_write_part(part, c, y);

            http::fields trailer;
            trailer.set("Hash", "hash_of_1234");

            part = HR::Trailer(move(trailer));
            rw.async_write_part(part, c, y);
        }
        outwc.wait(y);

        const string rsp =
            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2019 12:30:20 GMT\r\n"
            "Content-Type: text/html\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Trailer: Hash\r\n"
            "\r\n"
            "4\r\n"
            "1234\r\n"
            "0\r\n"
            "Hash: hash_of_1234\r\n"
            "\r\n";
        BOOST_REQUIRE_EQUAL(outs.str(), rsp);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_restart_body_body) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y_) {
        Cancel c;
        Yield y(ios, y_);

        stringstream outs;
        WaitCondition outwc(ios);
        {
            const string rb1("0123456789");

            http::response_header<> rh1;
            rh1.version(11);
            rh1.result(http::status::ok);
            rh1.set(http::field::date, "Mon, 27 Jul 2019 12:30:20 GMT");
            rh1.set(http::field::content_type, "text/html");
            rh1.set(http::field::content_length, rb1.size());

            const string rb2("abcde");

            http::response_header<> rh2;
            rh2.version(11);
            rh2.result(http::status::ok);
            rh2.set(http::field::date, "Mon, 27 Jul 2019 12:30:21 GMT");
            rh2.set(http::field::content_type, "text/html");
            rh2.set(http::field::content_length, rb2.size());

            RW rw(stream(outs, outwc, ios, y));
            HR::Part part;

            part = HR::Head(move(rh1));
            rw.async_write_part(part, c, y);

            part = HR::Body(true, str_to_vec(rb1));
            rw.async_write_part(part, c, y);

            part = HR::Head(move(rh2));
            rw.async_write_part(part, c, y);

            part = HR::Body(true, str_to_vec(rb2));
            rw.async_write_part(part, c, y);
        }
        outwc.wait(y);

        const string rsp =
            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2019 12:30:20 GMT\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 10\r\n"
            "\r\n"
            "0123456789"

            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2019 12:30:21 GMT\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "abcde";
        BOOST_REQUIRE_EQUAL(outs.str(), rsp);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_restart_chunks_body) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y_) {
        Cancel c;
        Yield y(ios, y_);

        stringstream outs;
        WaitCondition outwc(ios);
        {
            http::response_header<> rh1;
            rh1.version(11);
            rh1.result(http::status::ok);
            rh1.set(http::field::date, "Mon, 27 Jul 2019 12:30:20 GMT");
            rh1.set(http::field::content_type, "text/html");
            rh1.set(http::field::transfer_encoding, "chunked");

            const string rb2("abcde");

            http::response_header<> rh2;
            rh2.version(11);
            rh2.result(http::status::ok);
            rh2.set(http::field::date, "Mon, 27 Jul 2019 12:30:21 GMT");
            rh2.set(http::field::content_type, "text/html");
            rh2.set(http::field::content_length, rb2.size());

            RW rw(stream(outs, outwc, ios, y));
            HR::Part part;

            part = HR::Head(move(rh1));
            rw.async_write_part(part, c, y);

            part = HR::ChunkHdr(4, "");
            rw.async_write_part(part, c, y);

            part = HR::ChunkBody(str_to_vec("12"), 2);
            rw.async_write_part(part, c, y);

            part = HR::ChunkBody(str_to_vec("34"), 0);
            rw.async_write_part(part, c, y);

            part = HR::ChunkHdr(0, "");
            rw.async_write_part(part, c, y);

            part = HR::Trailer();
            rw.async_write_part(part, c, y);

            part = HR::Head(move(rh2));
            rw.async_write_part(part, c, y);

            part = HR::Body(true, str_to_vec(rb2));
            rw.async_write_part(part, c, y);
        }
        outwc.wait(y);

        const string rsp =
            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2019 12:30:20 GMT\r\n"
            "Content-Type: text/html\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4\r\n"
            "1234\r\n"
            "0\r\n"
            "\r\n"

            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2019 12:30:21 GMT\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "abcde";
        BOOST_REQUIRE_EQUAL(outs.str(), rsp);
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()
