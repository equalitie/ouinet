#pragma once

#include "namespaces.h"

#include <boost/system/error_code.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <functional>
#include <vector>
#include <iostream>
#include <util/executor.h>

namespace ouinet {

using ouinet::util::AsioExecutor;

class GenericStream {
public:
    using executor_type = AsioExecutor;

private:
    using OnRead  = std::function<void(sys::error_code, size_t)>;
    using OnWrite = std::function<void(sys::error_code, size_t)>;

    using ReadBuffers  = std::vector<asio::mutable_buffer>;
    using WriteBuffers = std::vector<asio::const_buffer>;

    struct Base {
        virtual executor_type get_executor() = 0;

        virtual void read_impl (OnRead&&)  = 0;
        virtual void write_impl(OnWrite&&) = 0;

        virtual void close() = 0;
        virtual bool is_open() const = 0;

        virtual ~Base() {}

        ReadBuffers  read_buffers;
        WriteBuffers write_buffers;
    };

    template<class Impl>
    struct Wrapper : public Base {
        using Shutter = std::function<void(Impl&)>;

        Wrapper(Impl&& impl)
            : _impl{std::move(impl)}
            , _shutter([](Impl& impl) { impl.close(); })
        {}

        Wrapper(Impl&& impl, Shutter shutter)
            : _impl{std::move(impl)}
            , _shutter(std::move(shutter))
        {}

        virtual executor_type get_executor() override
        {
            return _impl.get_executor();
        }

        void read_impl(OnRead&& on_read) override
        {
            _impl.async_read_some(read_buffers, std::move(on_read));
        }

        void write_impl(OnWrite&& on_write) override
        {
            _impl.async_write_some(write_buffers, std::move(on_write));
        }

        void close() override
        {
            if (_impl.is_open()) {
                _shutter(_impl);
            }
        }

        bool is_open() const override {
            return _impl.is_open();
        }

    private:
        Impl _impl;
        Shutter _shutter;
    };

public:
    using lowest_layer_type = GenericStream;

    GenericStream& lowest_layer() { return *this; }

    bool has_implementation() const { return _shared && _shared->impl; }

    void* id() const {
        if (!_shared) return nullptr;
        return _shared->impl.get();
    }

public:
    GenericStream() {}

    template<class AsyncRWStream>
    GenericStream(AsyncRWStream&& impl, std::string remote_ep = "")
        : _executor(impl.get_executor())
        , _shared(std::make_shared<Shared>(
                    new Wrapper<AsyncRWStream>(std::forward<AsyncRWStream>(impl))))
        , _remote_endpoint(std::move(remote_ep))
    { }

    template<class AsyncRWStream, class Shutter>
    GenericStream( AsyncRWStream&& impl
                 , Shutter shutter
                 , std::string remote_ep = "")
        : _executor(impl.get_executor())
        , _shared(std::make_shared<Shared>(
                    new Wrapper<AsyncRWStream>( std::forward<AsyncRWStream>(impl)
                                              , std::move(shutter))))
        , _remote_endpoint(std::move(remote_ep))
    {
    }

    GenericStream(GenericStream&& other) = default;

    GenericStream& operator=(GenericStream&& other) = default;

    ~GenericStream() {
        // Don't call explicit `close` on `_impl` here as that would interfere
        // with inner streams that only hold references to real streams.
        // Instead, call the underlying socket's destructor and let that decide
        // what to do.
        if (_shared) {
            _shared->impl = nullptr;
        }
    }

    executor_type get_executor()
    {
        return _executor;
    }

    void close()
    {
        if (!_shared || !_shared->impl) return;
        _shared->impl->close();
        _shared->impl = nullptr;
        _shared = nullptr;
    }

    bool is_open() const
    {
        if (!_shared || !_shared->impl) return false;
        return _shared->impl->is_open();
    }

    // Put data in the given buffers back into the read buffers,
    // so that it is returned on the next read operation.
    template<class ConstBufferSequence>
    void put_back(const ConstBufferSequence& bs, sys::error_code& ec)
    {
        if (!_shared || !_shared->impl) {
            ec = asio::error::bad_descriptor;
            return;
        }

        _shared->impl->read_buffers.resize(std::distance( asio::buffer_sequence_begin(bs)
                                                        , asio::buffer_sequence_end(bs)));

        std::copy( asio::buffer_sequence_begin(bs)
                 , asio::buffer_sequence_end(bs)
                 , _shared->impl->read_buffers.begin());
    }

    template< class MutableBufferSequence
            , class Token>
    auto async_read_some(const MutableBufferSequence& bs, Token&& token)
    {
        using namespace std;

        namespace asio   = boost::asio;
        namespace system = boost::system;

        auto init = [&](auto completion_handler) {
            if (!is_open()) {
                completion_handler(asio::error::bad_descriptor, 0);
                return;
            }

            {
                system::error_code ec;
                put_back(bs, ec);
                assert(!ec);
            }

            auto handler = make_shared<decltype(completion_handler)>(std::move(completion_handler));

            // TODO: It should not be necessary to check whether the underlying
            // implementation has been closed (Asio itself doesn't guarantee
            // returning an error in such cases). But it seems there may be a
            // bug in Boost.Beast (Boost v1.67) because even if it destroys the
            // socket it continues reading from it.
            // Test vector: uTP x TLS x bbc.com
            // (Same with the async_write_some operation)
            _shared->impl->read_impl([h = move(handler), shared = _shared]
                             (const system::error_code& ec, size_t size) {
                                 if (!shared->impl || !shared->impl->is_open()) {
                                    (*h)(asio::error::shut_down, 0);
                                 } else {
                                    (*h)(ec, size);
                                 }
                             });
        };

        return boost::asio::async_initiate<
            Token,
            void(boost::system::error_code, size_t)
          >(init, token);
    }

    template< class ConstBufferSequence
            , class Token>
    auto async_write_some(const ConstBufferSequence& bs, Token&& token)
    {
        using namespace std;

        namespace asio   = boost::asio;
        namespace system = boost::system;

        auto init = [&] (auto completion_handler) {
            if (!is_open()) {
                completion_handler(asio::error::bad_descriptor, 0);
                return;
            }

            auto handler = make_shared<decltype(completion_handler)>(std::move(completion_handler));

            _shared->impl->write_buffers.resize(distance( asio::buffer_sequence_begin(bs)
                                                        , asio::buffer_sequence_end(bs)));

            copy( asio::buffer_sequence_begin(bs)
                , asio::buffer_sequence_end(bs)
                , _shared->impl->write_buffers.begin());

            // TODO: Same as the comment in async_read_some operation
            _shared->impl->write_impl([h = move(handler), shared = _shared]
                              (const system::error_code& ec, size_t size) {
                                 if (!shared->impl || !shared->impl->is_open()) {
                                    (*h)(asio::error::shut_down, 0);
                                 } else {
                                    (*h)(ec, size);
                                 }
                              });
        };

        return boost::asio::async_initiate<
            Token,
            void(system::error_code, size_t)
        >(init, token);
    }

    const std::string& remote_endpoint() const { return _remote_endpoint; }

private:
    AsioExecutor _executor;

    struct Shared {
        std::unique_ptr<Base> impl;

        Shared(Base* impl) : impl(impl) {}
    };

    std::shared_ptr<Shared> _shared;
    std::string _remote_endpoint;
};

} // ouinet namespace
