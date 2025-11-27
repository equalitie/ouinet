#define BOOST_TEST_MODULE utility
#include <boost/test/included/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/endian.hpp>
#include <namespaces.h>
#include <iostream>
#include "generic_stream.h"
#include "connected_pair.h"

BOOST_AUTO_TEST_SUITE(ouinet_crypto_stream_tests)

using namespace ouinet;
using namespace std::string_literals;

// Sends each buffer sequence (blob) with a size prefix and breaks
// `async_read_some` calls on the blob boundary. Because HTTP often finds
// message boundaries by looking for '\r\n', it may read past it. Use this
// stream if you want to ensure that won't happen.
class BlobStream {
private:
    using BlobSizeType = uint16_t;

    struct Shared {
        std::optional<BlobSizeType> blob_size_rx;
        BlobSizeType blob_size_tx_be;
        GenericStream& stream;

        Shared(GenericStream& stream) : stream(stream) {}
    };

public:
    using executor_type = GenericStream::executor_type;

    BlobStream(GenericStream& stream):
        _executor(stream.get_executor()),
        _shared(std::make_shared<Shared>(stream))
    {}

    executor_type get_executor() {
        return _executor;
    }

    template< class ConstBufferSequence
            , class Token>
    auto async_write_some(const ConstBufferSequence& buffers, Token&& token) {
        using boost::endian::native_to_big;
        using boost::endian::big_to_native;

        enum Action { write_size, write_data, complete };

        return boost::asio::async_compose<
            Token,
            void(sys::error_code, size_t)
        >(
            [ shared = _shared,
              action = write_size,
              buffers
            ] (auto& self, sys::error_code ec = {}, size_t n = 0) mutable {
                switch (action) {
                    case write_size: {
                        BlobSizeType blob_size = std::min<size_t>(
                                std::numeric_limits<BlobSizeType>::max(),
                                asio::buffer_size(buffers));

                        shared->blob_size_tx_be = native_to_big(blob_size);

                        auto buffer = asio::buffer(&shared->blob_size_tx_be, sizeof(shared->blob_size_tx_be));

                        action = write_data;
                        asio::async_write(shared->stream, buffer, std::move(self));
                        break;
                    }
                    case write_data: {
                        if (ec) {
                            self.complete(ec, 0);
                            break;
                        }

                        action = complete;
                        auto blob_size = big_to_native(shared->blob_size_tx_be);

                        // Write the whole blob in one go because we already
                        // wrote the blob size.
                        asio::async_write(
                            shared->stream,
                            buffers,
                            asio::transfer_exactly(blob_size),
                            std::move(self)
                        );

                        break;
                    }
                    case complete:
                        self.complete(ec, n);
                        break;
                }
            },
            token,
            get_executor()         
        );
    }

    template< class MutableBufferSequence
            , class Token>
    auto async_read_some(const MutableBufferSequence& buffers, Token&& token) {
        using boost::endian::big_to_native;

        using Buffers = boost::asio::detail::consuming_buffers<
            asio::mutable_buffer,
            MutableBufferSequence,
            std::decay_t<decltype(asio::buffer_sequence_begin(buffers))>>;

        enum Action { convert_size, read_data, check_done };

        return boost::asio::async_compose<
            Token,
            void(sys::error_code, size_t)
        >(
            [ shared = _shared,
              action = read_data,
              buffers = Buffers(buffers)
            ] (auto& self, sys::error_code ec = {}, size_t n = 0) mutable {
                if (ec) {
                    self.complete(ec, n);
                    return;
                }

                if (!shared->blob_size_rx.has_value()) {
                    action = convert_size;
                    shared->blob_size_rx = {0};
                    auto buffer = asio::buffer(&*shared->blob_size_rx, sizeof(*shared->blob_size_rx));
                    asio::async_read(shared->stream, buffer, std::move(self));
                    return;
                }

                switch (action) {
                    case convert_size: {
                        shared->blob_size_rx = big_to_native(*shared->blob_size_rx);
                        action = read_data;
                        // No break, go to the `read_data` case directly.
                    }
                    case read_data: {
                        action = check_done;
                        shared->stream.async_read_some(buffers.prepare(*shared->blob_size_rx), std::move(self));
                        break;
                    }
                    case check_done:
                        action = read_data;
                        assert(*shared->blob_size_rx >= n);
                        *shared->blob_size_rx -= n;
                        if (*shared->blob_size_rx == 0) {
                            shared->blob_size_rx.reset();
                        }
                        self.complete(ec, n);
                        break;
                }
            },
            token,
            get_executor()         
        );
    }

private:
    executor_type _executor;
    std::shared_ptr<Shared> _shared;
};

template<size_t N>
std::array<uint8_t, N> generate_random_array() {
    std::array<uint8_t, N> array;
    if (RAND_bytes(array.data(), array.size()) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return array;
}

struct CryptoStreamKey : std::array<uint8_t, 32> {
    static CryptoStreamKey generate_random() {
        return CryptoStreamKey{generate_random_array<32>()};
    }
};

// Encrypt inner stream using AES 256 CTR mode. Note that CTR means that the
// stream is not authenticated. This is OK for our use case where we
// authenticate the plaintext using injector signatures.
template<class InnerStream>
class CryptoStream {
private:
    struct Iv : std::array<uint8_t, 16> {
        static Iv generate_random() {
            return Iv{generate_random_array<16>()};
        }
    
        std::array<uint8_t, 16> const& as_array() const { return *this; }
    };

    // TODO: What's the optimal buffer size?
    using BufferRx = std::array<uint8_t, 4096>;
    using BufferTx = std::array<uint8_t, 4096>;

    struct Shared {
        const EVP_CIPHER* cypher;
        BufferRx buffer_rx;
        BufferTx buffer_tx;
        CryptoStreamKey key;
        std::optional<Iv> encrypt_iv; // Lazily initialized
        std::optional<Iv> decrypt_iv; // Received
        EVP_CIPHER_CTX* encrypt_ctx = nullptr;
        EVP_CIPHER_CTX* decrypt_ctx = nullptr;
        InnerStream& stream;

        Shared(InnerStream& stream, CryptoStreamKey const& key):
            cypher(EVP_aes_256_ctr()),
            key(key),
            stream(stream)
        {
            // Should be the case for CTR mode and is assumed in the
            // encryption/decryption code.
            assert(EVP_CIPHER_block_size(cypher) == 1);
        }

        ~Shared() {
            if (encrypt_ctx) EVP_CIPHER_CTX_free(encrypt_ctx);
            if (decrypt_ctx) EVP_CIPHER_CTX_free(decrypt_ctx);
        }
    };

public:
    using executor_type = InnerStream::executor_type;

    CryptoStream(InnerStream& stream, CryptoStreamKey const& key):
        _executor(stream.get_executor()),
        _shared(std::make_shared<Shared>(stream, key))
    {}

    executor_type get_executor() {
        return _executor;
    }

    template< class ConstBufferSequence
            , class Token>
    auto async_write_some(const ConstBufferSequence& inbufs, Token&& token) {
        return asio::async_compose<Token, void(sys::error_code, size_t)>(
            [ shared = _shared,
              inbufs,
              finish = false
            ]
            (auto& self, sys::error_code ec = {}, size_t n = 0) mutable {
                if (finish) {
                    self.complete(ec, n);
                    return;
                }

                if (!shared->encrypt_iv) {
                    shared->encrypt_iv = Iv::generate_random();
                    asio::async_write(shared->stream, asio::buffer(shared->encrypt_iv->as_array()), std::move(self));
                    return;
                }

                if (!shared->encrypt_ctx) {
                    shared->encrypt_ctx = EVP_CIPHER_CTX_new();
                    if (!EVP_EncryptInit_ex(shared->encrypt_ctx, shared->cypher, NULL, shared->key.data(), shared->encrypt_iv->data()))
                        assert(false);
                }

                auto& outbuf = shared->buffer_tx;
                const size_t size = std::min(outbuf.size(), asio::buffer_size(inbufs));
                size_t wrote = 0;

                for (auto inbuf_i = asio::buffer_sequence_begin(inbufs);
                        inbuf_i != asio::buffer_sequence_end(inbufs);
                        ++inbuf_i) {
                    auto& inbuf = *inbuf_i;

                    int outlen;
                    int count = std::min(size - wrote, inbuf.size());

                    if (!EVP_EncryptUpdate(shared->encrypt_ctx, outbuf.data() + wrote, &outlen, static_cast<const unsigned char*>(inbuf.data()), count))
                        assert(false);

                    assert(count == outlen && "must hold because block size of this cypher is 1");
                    wrote += count;

                    if (wrote == size) break;
                }

                finish = true;
                asio::async_write(shared->stream, asio::buffer(outbuf.data(), wrote), std::move(self));
            },
            token,
            get_executor()         
        );

    }

    template< class MutableBufferSequence
            , class Token>
    auto async_read_some(const MutableBufferSequence& buffers, Token&& token) {
        enum Action { receive, decrypt };

        return asio::async_compose<Token, void(sys::error_code, size_t)>(
            [ shared = _shared,
              action = receive,
              buffers
            ]
            (auto& self, sys::error_code ec = {}, size_t n = 0) mutable {
                if (ec) {
                    self.complete(ec, n);
                    return;
                }

                if (!shared->decrypt_iv.has_value()) {
                    shared->decrypt_iv = Iv{};
                    asio::async_read(shared->stream, asio::buffer(*shared->decrypt_iv), std::move(self));
                    return;
                }

                if (!shared->decrypt_ctx) {
                    shared->decrypt_ctx = EVP_CIPHER_CTX_new();
                    if (!EVP_DecryptInit_ex(
                            shared->decrypt_ctx,
                            shared->cypher,
                            NULL, shared->key.data(),
                            shared->decrypt_iv->data()))
                        assert(false);
                }

                switch (action) {
                    case receive: {
                        action = decrypt;
                        auto max = std::min(shared->buffer_rx.size(), asio::buffer_size(buffers));
                        shared->stream.async_read_some(asio::buffer(shared->buffer_rx.data(), max), std::move(self));
                        return;
                    }
                    case decrypt: {
                        size_t to_decrypt = n;

                        for (auto outbuf_i = asio::buffer_sequence_begin(buffers);
                                outbuf_i != asio::buffer_sequence_end(buffers);
                                ++outbuf_i) {
                            if (to_decrypt == 0) break;
                            int max = std::min(to_decrypt, outbuf_i->size());
                            int outlen;
                            size_t offset = n - to_decrypt;
                            if (!EVP_DecryptUpdate(
                                    shared->decrypt_ctx,
                                    static_cast<unsigned char*>(outbuf_i->data()),
                                    &outlen,
                                    shared->buffer_rx.data() + offset,
                                    max))
                                assert(false);

                            assert(outlen == max && "must hold because block size of this cypher is 1");

                            to_decrypt -= outlen;
                        }

                        self.complete(ec, n);
                    }
                }
            },
            token,
            get_executor()         
        );
    }

private:
    executor_type _executor;
    std::shared_ptr<Shared> _shared;
};

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

        auto s1 = CryptoStream<S>(socket1, key);
        auto s2 = CryptoStream<S>(socket2, key);

        test_all_cases(s1, s2, yield);
    },
    check_exception);

    ctx.run();
}

BOOST_AUTO_TEST_SUITE_END()

