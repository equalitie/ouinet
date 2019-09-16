#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/static_buffer.hpp>

namespace ouinet { namespace util {

// A silly wrapper class over a buffer
// that allows arbitrary writes but only
// fixed-size, one piece contiguous buffer reads
// when at least N bytes have been put in the buffer.
template <std::size_t N>
class quantized_buffer {
    boost::beast::static_buffer<2 * N> buf;

public:
    // May throw `std::length_error`.
    template <class ConstBufferSequence>
    std::size_t put(const ConstBufferSequence& source)
    {
        auto w = boost::asio::buffer_copy(buf.prepare(source.size()), source);
        buf.commit(w);
        return w;
    }

    // May throw `std::length_error`.
    template <class ConstBufferSequence>
    std::size_t put(const ConstBufferSequence& source, std::size_t n)
    {
        auto w = boost::asio::buffer_copy(buf.prepare(n), source, n);
        buf.commit(w);
        return w;
    }

    // Get a quantum of data
    // if enough data is available for reading in the buffer,
    // otherwise just get an empty buffer.
    //
    // Note: Putting data into the buffer after this operation
    // may overwrite the data in the returned buffer.
    boost::asio::const_buffer get() noexcept
    {
        if (buf.size() < N)
            return boost::asio::const_buffer();  // not enough data yet

        auto data = buf.data();
        auto data0 = boost::asio::buffer_sequence_begin(data);
        assert(data0->size() >= N);
        boost::asio::const_buffer ret{data0->data(), N};
        buf.consume(N);
        return ret;
    }

    // Use this to retrieve the rest of data
    // if `get` still returns an empty buffer.
    //
    // The buffer is cleared.
    boost::asio::const_buffer get_rest() noexcept
    {
        assert(buf.size() < N);  // use `get` otherwise
        auto data = buf.data();
        auto data0 = boost::asio::buffer_sequence_begin(data);
        assert(data0->size() == buf.size());
        boost::asio::const_buffer ret{data0->data(), data0->size()};
        buf.consume(buf.size());
        return ret;
    }
};

}} // namespaces
