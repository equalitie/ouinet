#pragma once

#include <boost/endian.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/compose.hpp>
#include "crypto_stream_key.h"
#include "generic_stream.h"

namespace ouinet {

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

// Encrypt inner stream using AES 256 CTR mode. Note that CTR means that the
// stream is not authenticated. This is OK for our use case where we
// authenticate the plaintext using injector signatures.
template<class InnerStream>
class CryptoStream {
private:
    struct Iv : std::array<uint8_t, 16> {
        static sys::result<Iv> generate_random() {
            auto array = detail::generate_random_array<16>();
            if (!array) return array.error();
            return Iv{*array};
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
        InnerStream stream;

        Shared(InnerStream&& stream, CryptoStreamKey const& key):
            cypher(EVP_aes_256_ctr()),
            key(key),
            stream(std::move(stream))
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

    CryptoStream(InnerStream stream, CryptoStreamKey const& key):
        _executor(stream.get_executor()),
        _shared(std::make_shared<Shared>(std::move(stream), key))
    {}

    CryptoStream(CryptoStream&&) = default;

    executor_type get_executor() {
        return _executor;
    }

    void close() {
        if (!is_open()) return;
        _shared->stream.close();
    }

    bool is_open() const {
        if (!_shared) return false;
        return _shared->stream.is_open();
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
                    auto iv = Iv::generate_random();
                    if (!iv) {
                        self.complete(iv.error(), n);
                        return;
                    }
                    shared->encrypt_iv = *iv;
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

    InnerStream* inner() {
        if (!_shared) return nullptr; // Was moved from
        return &_shared->stream;
    } 

private:
    executor_type _executor;
    std::shared_ptr<Shared> _shared;
};

template<class InnerStream>
class StreamRef {
public:
    using executor_type = InnerStream::executor_type;

    StreamRef(InnerStream& inner):
        _executor(inner.get_executor()),
        _inner(inner)
    {}

    StreamRef(StreamRef const& other):
        _executor(other._executor),
        _inner(const_cast<InnerStream&>(other._inner))
    {}

    executor_type get_executor() {
        return _executor;
    }

    void close() {
        _inner.close();
    }

    bool is_open() const {
        return _inner.is_open();
    }

    template< class ConstBufferSequence
            , class Token>
    auto async_write_some(const ConstBufferSequence& inbufs, Token&& token) {
        return _inner.async_write_some(inbufs, std::forward<Token>(token));
    }

    template< class MutableBufferSequence
            , class Token>
    auto async_read_some(const MutableBufferSequence& buffers, Token&& token) {
        return _inner.async_read_some(buffers, std::forward<Token>(token));
    }

    InnerStream& inner() {
        return _inner;
    } 

private:
    executor_type _executor;
    InnerStream& _inner;
};

} // namespace
