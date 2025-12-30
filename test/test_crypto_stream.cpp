#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/endian.hpp>
#include <namespaces.h>
#include <iostream>
#include "generic_stream.h"
#include "connected_pair.h"
#include "util/crypto_stream.h"

BOOST_AUTO_TEST_SUITE(ouinet_crypto_stream_tests)

using namespace ouinet;
using namespace std::string_literals;

// Test fixtures

const std::vector<std::string> test_buffers = {
    "brown"s, "fox"s, "jumps"s, "over"s, "the"s, "lazy"s, "dog"s
};
 
template<class Sequence>
std::string concatenate(Sequence const& blobs) {
    std::string concatenated;
    for (auto& blob : test_buffers) concatenated += blob;
    return concatenated;
}

// Test cases

template<class StreamType>
void case_write_call_n_buffer_1___read_call_1_buffer_1(StreamType& s1, StreamType& s2, asio::yield_context yield) {
    for (auto& blob : test_buffers) {
        asio::async_write(s1, asio::buffer(blob), yield);
    }
    
    auto concatenated = concatenate(test_buffers);
    
    std::string buffer(concatenated.size(), 0);
    asio::async_read(s2, asio::buffer(buffer), yield);
    
    BOOST_REQUIRE_EQUAL(concatenated, buffer);
}

template<class StreamType>
void case_write_call_n_buffer_1___read_call_n_buffer_1(StreamType& s1, StreamType& s2, asio::yield_context yield) {
    for (auto& blob : test_buffers) {
        asio::async_write(s1, asio::buffer(blob), yield);
    }
    
    auto concatenated = concatenate(test_buffers);
    
    std::string received;
    while (received.size() != concatenated.size()) {
        size_t buf_size = std::min<size_t>(3, concatenated.size() - received.size());
        std::string buffer(buf_size, 0);
        asio::async_read(s2, asio::buffer(buffer), yield);
        received += buffer;
    }
    
    BOOST_REQUIRE_EQUAL(concatenated, received);
}

template<class StreamType>
void case_write_call_1_buffer_n___read_call_1_buffer_n(StreamType& s1, StreamType& s2, asio::yield_context yield) {
    std::vector<asio::const_buffer> buffers;

    for (auto& blob : test_buffers) {
        buffers.push_back(asio::buffer(blob));
    }
    
    asio::async_write(s1, buffers, yield);

    auto concatenated_tx = concatenate(test_buffers);
    
    std::array<std::string, 2> halves = {
        std::string(concatenated_tx.size() / 2, 0),
        std::string(concatenated_tx.size() / 2 + concatenated_tx.size() % 2, 0),
    };

    std::array<asio::mutable_buffer, 2> buffers_rx {
        asio::buffer(halves[0]),
        asio::buffer(halves[1])
    };

    asio::async_read(s2, buffers_rx, yield);
    
    BOOST_REQUIRE_EQUAL(concatenated_tx, concatenate(halves));
}

void check_exception(std::exception_ptr e) {
    try {
        if (e) {
            std::rethrow_exception(e);
        }
    } catch (const std::exception& e) {
        BOOST_FAIL("Test failed with exception: " << e.what());
    } catch (...) {
        BOOST_FAIL("Test failed with unknown exception");
    }
}

template<class StreamType>
void test_all_cases(StreamType& s1, StreamType& s2, asio::yield_context yield) {
    case_write_call_n_buffer_1___read_call_1_buffer_1(s1, s2, yield);
    case_write_call_n_buffer_1___read_call_n_buffer_1(s1, s2, yield);
    case_write_call_1_buffer_n___read_call_1_buffer_n(s1, s2, yield);
}

// Tests

BOOST_AUTO_TEST_CASE(test_blob_stream) {
    asio::io_context ctx;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        auto [socket1, socket2] = util::connected_pair(yield);

        auto gs1 = GenericStream(std::move(socket1));
        auto gs2 = GenericStream(std::move(socket2));

        auto s1 = BlobStream(gs1);
        auto s2 = BlobStream(gs2);

        test_all_cases(s1, s2, yield);
    },
    check_exception);

    ctx.run();
}

BOOST_AUTO_TEST_CASE(test_crypto_stream) {
    asio::io_context ctx;

    asio::spawn(ctx, [&] (asio::yield_context yield) {
        auto [socket1, socket2] = util::connected_pair(yield);

        using S = asio::ip::tcp::socket;

        auto key = CryptoStreamKey::generate_random();

        auto s1 = CryptoStream<S>(std::move(socket1), key);
        auto s2 = CryptoStream<S>(std::move(socket2), key);

        test_all_cases(s1, s2, yield);
    },
    check_exception);

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()

