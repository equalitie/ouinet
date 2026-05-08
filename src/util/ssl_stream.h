#pragma once

#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/compose.hpp>
#include "namespaces.h"

namespace ouinet {

/*
 * Wrapper over asio::ssl::stream which has properties more similar to a normal
 * {tcp,udp}::socket. Namely:
 *
 *   1. It's move-able
 *   2. Can be destroyed while async operations are running and the destructor
 *      cancels those operations
 */
template<class InnerStream>
class SslStream {
public:
    using executor_type = InnerStream::executor_type;

    explicit SslStream(InnerStream inner, asio::ssl::context& ssl_context):
        _executor(inner.get_executor()),
        _shared(std::make_shared<asio::ssl::stream<InnerStream>>(std::move(inner), ssl_context))
    {}

    SslStream(SslStream&&) = default;

    executor_type get_executor() {
        return _executor;
    }

          asio::ssl::stream<InnerStream>* operator->()       { return _shared.get(); }
    const asio::ssl::stream<InnerStream>* operator->() const { return _shared.get(); }

    template< class ConstBufferSequence
            , class Token>
    auto async_write_some(const ConstBufferSequence& buffers, Token&& token) {
        enum Action { write, complete };

        return boost::asio::async_compose<
            Token,
            void(sys::error_code, size_t)
        >(
            [ shared = _shared,
              action = write,
              buffers
            ] (auto& self, sys::error_code ec = {}, size_t n = 0) mutable {
                switch (action) {
                    case write:
                        if (!shared) {
                            self.complete(asio::error::bad_descriptor, 0);
                            break;
                        }
                        action = complete;
                        shared->async_write_some(buffers, move(self));
                        break;
                    case complete:
                        if (!shared->next_layer().is_open()) {
                            ec = asio::error::shut_down;
                        }
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
        enum Action { read, complete };

        return boost::asio::async_compose<
            Token,
            void(sys::error_code, size_t)
        >(
            [ shared = _shared,
              action = read,
              buffers
            ] (auto& self, sys::error_code ec = {}, size_t n = 0) mutable {
                switch (action) {
                    case read:
                        if (!shared) {
                            self.complete(asio::error::bad_descriptor, 0);
                            break;
                        }
                        action = complete;
                        shared->async_read_some(buffers, move(self));
                        break;
                    case complete:
                        if (!shared->next_layer().is_open()) {
                            ec = asio::error::shut_down;
                        }
                        self.complete(ec, n);
                        break;
                }
            },
            token,
            get_executor()         
        );
    }

    void close() {
        if (!is_open()) return;
        _shared->next_layer().close();
    }

    bool is_open() const {
        if (!_shared) return false; // Was moved from
        return _shared->next_layer().is_open();
    }

    ~SslStream() {
        close();
    }

public:
    executor_type _executor;
    std::shared_ptr<asio::ssl::stream<InnerStream>> _shared;
};

} // namespace
