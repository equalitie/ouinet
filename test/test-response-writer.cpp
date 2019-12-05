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

BOOST_AUTO_TEST_CASE(test_simple) {
    asio::io_service ios;

    asio::spawn(ios, [&] (auto y_) {
        Cancel c;
        Yield y(ios, y_);

        stringstream outs;
        WaitCondition outwc(ios);
        {
            RW rw(stream(outs, outwc, ios, y));

            const string rb("an example body");

            http::response_header<> rh;
            rh.version(11);
            rh.result(http::status::ok);
            rh.set(http::field::content_length, rb.size());

            HR::Part part;

            part = HR::Head(move(rh));
            rw.async_write_part(part, c, y);

            part = HR::Body(true, str_to_vec(rb));
            rw.async_write_part(part, c, y);
        }
        outwc.wait(y);

        const string rs =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 15\r\n"
            "\r\n"
            "an example body";
        BOOST_REQUIRE_EQUAL(outs.str(), rs);
    });

    ios.run();
}

BOOST_AUTO_TEST_SUITE_END()
