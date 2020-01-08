#define BOOST_TEST_MODULE response_reader
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/optional/optional_io.hpp>
#include "../src/or_throw.h"
#include "../src/response_reader.h"
#include "../src/util/wait_condition.h"

using namespace std;
using namespace ouinet;

using tcp = asio::ip::tcp;
using RR = http_response::Reader;

namespace HR = http_response;

// TODO: There should be a more straight forward way to do this.
tcp::socket
stream(string response, asio::io_service& ios, asio::yield_context yield)
{
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

    asio::spawn(ios, [rsp = move(response), s = move(s2)]
                     (asio::yield_context yield) mutable {
            asio::async_write(s, asio::buffer(rsp), yield);
        });

    return s1;
}

vector<uint8_t> str_to_vec(boost::string_view s) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
    return {p, p + s.size()};
}

string vec_to_str(const vector<uint8_t>& v) {
    const char* p = reinterpret_cast<const char*>(v.data());
    return {p, v.size()};
}

HR::Part body(boost::string_view s) {
    return HR::Body(str_to_vec(s));
}

HR::Part chunk_body(boost::string_view s) {
    return HR::ChunkBody(str_to_vec(s), 0);
}

HR::Part chunk_hdr(size_t size, boost::string_view s) {
    return HR::ChunkHdr{size, s.to_string()};
}

HR::Part trailer(map<string, string> trailer) {
    http::fields fields;
    for (auto& p : trailer) {
        fields.insert(p.first, p.second);
    }
    return HR::Trailer{move(fields)};
}

namespace ouinet { namespace http_response {
    std::ostream& operator<<(std::ostream& os, const HR::Head&) {
        return os << "Head";
    }

    std::ostream& operator<<(std::ostream& os, const HR::ChunkHdr& hdr) {
        return os << "ChunkHdr(" << hdr.size << " exts:\"" << hdr.exts << "\")";
    }

    std::ostream& operator<<(std::ostream& os, const HR::ChunkBody& b) {
        return os << "ChunkBody(" << vec_to_str(b) << ")";
    }

    std::ostream& operator<<(std::ostream& os, const HR::Body& b) {
        return os << "Body(" << vec_to_str(b) << ")";
    }

    std::ostream& operator<<(std::ostream& os, const HR::Trailer&) {
        return os << "Trailer";
    }
}} // ouinet namespaces::http_response

HR::Part read_full_body(RR& rr, Cancel& c, asio::yield_context y) {
    HR::Body body({});

    while (true) {
        sys::error_code ec;
        auto opt_part = rr.async_read_part(c, y[ec]);
        if (!opt_part) break;
        BOOST_REQUIRE(!ec);
        auto body_p = opt_part->as_body();
        BOOST_REQUIRE(body_p);
        body.insert(body.end(), body_p->begin(), body_p->end());
    }

    return body;
}

BOOST_AUTO_TEST_SUITE(ouinet_response_reader)

BOOST_AUTO_TEST_CASE(test_http10_no_body) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y) {
        string rsp =
            "HTTP/1.0 200 OK\r\n"
            "\r\n";

        RR rr(stream(move(rsp), ios, y));

        Cancel c;
        boost::optional<HR::Part> part;

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE(part->is_head());

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(!part);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http10_body_no_length) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y) {
        string rsp =
            "HTTP/1.0 200 OK\r\n"
            "\r\n"
            "abcdef";

        RR rr(stream(move(rsp), ios, y));

        Cancel c;
        boost::optional<HR::Part> part;

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE(part->is_head());

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, body("abcdef"));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(!part);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_no_body) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y) {
        string rsp =
            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2019 12:30:20 GMT\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 0\r\n"
            "\r\n";

        RR rr(stream(move(rsp), ios, y));

        Cancel c;
        boost::optional<HR::Part> part;

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE(part->is_head());

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(!part);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_body) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y) {
        string rsp =
            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2019 12:30:20 GMT\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 10\r\n"
            "\r\n"
            "0123456789";

        RR rr(stream(move(rsp), ios, y));

        Cancel c;
        boost::optional<HR::Part> part;

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE(part->is_head());

        part = read_full_body(rr, c, y);
        BOOST_REQUIRE_EQUAL(part, body("0123456789"));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(!part);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_chunk) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y) {
        string rsp =
            "HTTP/1.1 200 OK\r\n"
            "Date: Mon, 27 Jul 2019 12:30:20 GMT\r\n"
            "Content-Type: text/html\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4\r\n"
            "1234\r\n"
            "0\r\n"
            "\r\n";

        RR rr(stream(move(rsp), ios, y));

        Cancel c;
        boost::optional<HR::Part> part;

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE(part->is_head());

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, chunk_hdr(4, ""));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, chunk_body("1234"));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, chunk_hdr(0, ""));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, trailer({}));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(!part);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_trailer) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y) {
        string rsp =
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

        RR rr(stream(move(rsp), ios, y));

        Cancel c;
        boost::optional<HR::Part> part;

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE(part->is_head());

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, chunk_hdr(4, ""));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, chunk_body("1234"));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, chunk_hdr(0, ""));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, trailer({{"Hash", "hash_of_1234"}}));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(!part);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_restart_body_body) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y) {
        string rsp =
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

        RR rr(stream(move(rsp), ios, y));

        Cancel c;
        boost::optional<HR::Part> part;

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE(part->is_head());

        part = read_full_body(rr, c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, body("0123456789"));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(!part);

        rr.restart();

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE(part->is_head());

        part = read_full_body(rr, c, y);
        BOOST_REQUIRE(part);
        BOOST_REQUIRE_EQUAL(*part, body("abcde"));

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(!part);
    });

    ios.run();
}

BOOST_AUTO_TEST_CASE(test_http11_restart_chunks_body) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y) {
        string rsp =
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

        RR rr(stream(move(rsp), ios, y));

        Cancel c;
        boost::optional<HR::Part> part;

        {
            part = rr.async_read_part(c, y);
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_head());

            part = rr.async_read_part(c, y);
            BOOST_REQUIRE(part);
            BOOST_REQUIRE_EQUAL(*part, chunk_hdr(4, ""));

            part = rr.async_read_part(c, y);
            BOOST_REQUIRE(part);
            BOOST_REQUIRE_EQUAL(*part, chunk_body("1234"));

            part = rr.async_read_part(c, y);
            BOOST_REQUIRE(part);
            BOOST_REQUIRE_EQUAL(*part, chunk_hdr(0, ""));

            part = rr.async_read_part(c, y);
            BOOST_REQUIRE(part);
            BOOST_REQUIRE_EQUAL(*part, trailer({}));
        }

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(!part);

        rr.restart();

        {
            part = rr.async_read_part(c, y);
            BOOST_REQUIRE(part);
            BOOST_REQUIRE(part->is_head());

            part = read_full_body(rr, c, y);
            BOOST_REQUIRE(part);
            BOOST_REQUIRE_EQUAL(*part, body("abcde"));
        }

        part = rr.async_read_part(c, y);
        BOOST_REQUIRE(!part);
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()


