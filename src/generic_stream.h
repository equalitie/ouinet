#pragma once

#include "namespaces.h"

#include <boost/system/error_code.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/post.hpp>
#include <functional>
#include <vector>
#include <iostream>

namespace ouinet {

namespace generic_stream_detail {
    // Some stream implementations (such as the asio::ssl::stream in Boost
    // <=1.67.0) are not movable. This template specialization shall allow us
    // to move std::unique_ptr<NonMovableStream> into GenericStream instead
    // of NonMovableStream&& directly.
    template<class T> struct Deref {
        using type = T;

              T& operator*()        { return value; }
        const T& operator*()  const { return value; }
              T* operator->()       { return &value; }
        const T* operator->() const { return &value; }

        T value;
    };

    template<class T> struct Deref<std::unique_ptr<T>> {
        using type = T;

              T& operator*()        { return *value; }
        const T& operator*()  const { return *value; }
              T* operator->()       { return value.get(); }
        const T* operator->() const { return value.get(); }

        std::unique_ptr<T> value;
    };

    template<class T> T& deref(T& v) { return v; }
    template<class T> T& deref(std::unique_ptr<T>& v) { return *v; }

    template<class T> bool is_open(const T& v) { return v.is_open(); }
    template<class T> bool is_open(const asio::ssl::stream<T>& v) { return v.next_layer().is_open(); }
} // namespace


class GenericStream {
public:
#if BOOST_VERSION >= 107100
    using executor_type = boost::asio::executor;
#elif BOOST_VERSION >= 106700
    using executor_type = asio::io_context::executor_type;
#else
   template<class Token, class Ret>
    using Handler = typename asio::handler_type< Token
                                                , void(sys::error_code, Ret)
                                                >::type;

     template<class Token, class Ret>
     using Result = typename asio::async_result<Handler<Token, Ret>>;
#endif

private:
    using OnRead  = std::function<void(sys::error_code, size_t)>;
    using OnWrite = std::function<void(sys::error_code, size_t)>;

    using ReadBuffers  = std::vector<asio::mutable_buffer>;
    using WriteBuffers = std::vector<asio::const_buffer>;

    struct Base {
#if BOOST_VERSION >= 106700
        virtual executor_type     get_executor() = 0;
#else
        virtual asio::io_service& get_io_service() = 0;
#endif

        virtual void read_impl (OnRead&&)  = 0;
        virtual void write_impl(OnWrite&&) = 0;

        virtual void close() = 0;
        virtual bool closed() const = 0;
        virtual bool is_open() const = 0;

        virtual ~Base() {}

        ReadBuffers  read_buffers;
        WriteBuffers write_buffers;
    };

    template<class Impl>
    struct Wrapper : public Base {
        using Shutter = std::function<
            void(typename generic_stream_detail::Deref<Impl>::type&)>;

        Wrapper(Impl&& impl)
            : _impl{std::move(impl)}
            , _shutter([](Impl& impl) { impl.close(); })
        {}

        Wrapper(Impl&& impl, Shutter shutter)
            : _impl{std::move(impl)}
            , _shutter(std::move(shutter))
        {}


#if BOOST_VERSION >= 106700
        virtual executor_type get_executor() override
        {
            return _impl->get_executor();
        }
#else
        virtual asio::io_service& get_io_service() override
        {
            return _impl->get_io_service();
        }
#endif

        void read_impl(OnRead&& on_read) override
        {
            _impl->async_read_some(read_buffers, std::move(on_read));
        }

        void write_impl(OnWrite&& on_write) override
        {
            _impl->async_write_some(write_buffers, std::move(on_write));
        }

        void close() override
        {
            _closed = true;
            _shutter(*_impl);
        }

        bool closed() const override {
            return _closed;
        }

        bool is_open() const override {
            if (_closed) return false;
            return generic_stream_detail::is_open(*_impl);
        }

    private:
        generic_stream_detail::Deref<Impl> _impl;
        Shutter _shutter;
        bool _closed = false;
    };

public:
    using lowest_layer_type = GenericStream;

    GenericStream& lowest_layer() { return *this; }

    bool has_implementation() const { return _impl != nullptr; }

    void* id() const { return _impl.get(); }

public:
    GenericStream() {
        if (_debug) {
            std::cerr << this << " " << _impl
                      << " GenericStream::destroy_implementation()"
                      << std::endl;
        }
    }

    template<class AsyncRWStream>
    GenericStream(AsyncRWStream&& impl, std::string remote_ep = "")
        : _executor(impl.get_executor())
        , _impl(new Wrapper<AsyncRWStream>(std::forward<AsyncRWStream>(impl)))
        , _remote_endpoint(std::move(remote_ep))
    {
        if (_debug) {
            std::cerr << this << " " << (void*)nullptr
                      << " GenericStream::GenericStream(&& "
                      << typeid(AsyncRWStream).name() << " "
                      << _impl << ")" << std::endl;
        }
    }

    template<class AsyncRWStream, class Shutter>
    GenericStream( AsyncRWStream&& impl
                 , Shutter shutter
                 , std::string remote_ep = "")
        : _executor(generic_stream_detail::deref(impl).get_executor())
        , _impl(new Wrapper<AsyncRWStream>( std::forward<AsyncRWStream>(impl)
                                          , std::move(shutter)))
        , _remote_endpoint(std::move(remote_ep))
    {
        if (_debug) {
            std::cerr << this << " " << (void*)nullptr
                      << " GenericStream::GenericStream(&& "
                      << typeid(AsyncRWStream).name() << " "
                      << _impl <<  ", shutter)" << std::endl;
        }
    }

    GenericStream(GenericStream&& other)
        : _executor(std::move(other._executor))
        , _impl(std::move(other._impl))
        , _remote_endpoint(std::move(other._remote_endpoint))
    {
        if (_debug) {
            std::cerr << this << " " << (void*)nullptr
                      << " GenericStream::GenericStream(&& "
                      << &other << " " << _impl <<  ")" << std::endl;
        }
    }

    GenericStream& operator=(GenericStream&& other) {
        _executor = std::move(other._executor);
        _remote_endpoint = std::move(other._remote_endpoint);

        if (_debug) {
            std::cerr << this << " " << _impl
                      << " GenericStream::operator=("
                      << &other << " " << other._impl <<  ")" << std::endl;
        }

        _impl = std::move(other._impl);
        return *this;
    }

    ~GenericStream() {
        if (_debug) {
            std::cerr << this << " " << _impl
                      << " GenericStream::~GenericStream()" << std::endl;
        }
        try {
            if (_impl) {
                _impl->close();
            }
        }
        catch (...) {
            assert(0 && "Uncaught exception when closing GenericStream");
        }
    }

    //asio::io_service& get_io_service()
    //{
    //    return *_ios;
    //}

#if BOOST_VERSION >= 107100
    executor_type get_executor()
    {
        return _executor;
    }
#elif BOOST_VERSION >= 106700
    executor_type get_executor()
    {
        assert(_impl);
        return _impl->get_executor();
    }
#endif

    void close()
    {
        if (_debug) {
            std::cerr << this << " " << _impl
                      << " GenericStream::close()" << std::endl;
        }
        if (!_impl) return;
        _impl->close();
        _impl = nullptr;
    }

    bool is_open() const
    {
        if (!_impl) return false;
        return _impl->is_open();
    }

    // Put data in the given buffers back into the read buffers,
    // so that it is returned on the next read operation.
    template<class ConstBufferSequence>
    void put_back(const ConstBufferSequence& bs, sys::error_code& ec)
    {
        if (!_impl) {
            ec = asio::error::bad_descriptor;
            return;
        }

        _impl->read_buffers.resize(std::distance( asio::buffer_sequence_begin(bs)
                                                , asio::buffer_sequence_end(bs)));

        std::copy( asio::buffer_sequence_begin(bs)
                 , asio::buffer_sequence_end(bs)
                 , _impl->read_buffers.begin());
    }

    template< class MutableBufferSequence
            , class Token>
    auto async_read_some(const MutableBufferSequence& bs, Token&& token)
    {
        if (_debug) {
            std::cerr << this << " " << _impl
                      << " GenericStream::async_read_some()" << std::endl;
        }

        using namespace std;

        namespace asio   = boost::asio;
        namespace system = boost::system;

        using Sig = void(system::error_code, size_t);

        boost::asio::async_completion<Token, Sig> init(token);

        using Handler = std::decay_t<decltype(init.completion_handler)>;

        // XXX: Handler is non-copyable, but can we do this without allocation?
        auto handler = make_shared<Handler>(std::move(init.completion_handler));

        if (_impl) {
            {
                system::error_code ec;
                put_back(bs, ec);
                assert(!ec);
            }

            // TODO: It should not be necessary to check whether the underlying
            // implementation has been closed (Asio itself doesn't guarantee
            // returning an error in such cases). But it seems there may be a
            // bug in Boost.Beast (Boost v1.67) because even if it destroys the
            // socket it continues reading from it.
            // Test vector: uTP x TLS x bbc.com
            // (Same with the async_write_some operation)
            _impl->read_impl([h = move(handler), impl = _impl]
                             (const system::error_code& ec, size_t size) {
                                 if (impl->closed()) {
                                    (*h)(asio::error::operation_aborted, 0);
                                 } else {
                                    (*h)(ec, size);
                                 }
                             });
        }
        else {
            asio::post(_executor, [h = move(handler)]
                                  { (*h)(asio::error::bad_descriptor, 0); });
        }

        return init.result.get();
    }

    template< class ConstBufferSequence
            , class Token>
    auto async_write_some(const ConstBufferSequence& bs, Token&& token)
    {
        if (_debug) {
            std::cerr << this << " " << _impl
                      << " GenericStream::async_write_some()" << std::endl;
        }

        using namespace std;

        namespace asio   = boost::asio;
        namespace system = boost::system;

        using Sig = void(system::error_code, size_t);

        boost::asio::async_completion<Token, Sig> init(token);

        using Handler = std::decay_t<decltype(init.completion_handler)>;

        // XXX: Handler is non-copyable, but can we do this without allocation?
        auto handler = make_shared<Handler>(std::move(init.completion_handler));

        if (_impl) {
            _impl->write_buffers.resize(distance( asio::buffer_sequence_begin(bs)
                                                , asio::buffer_sequence_end(bs)));

            copy( asio::buffer_sequence_begin(bs)
                , asio::buffer_sequence_end(bs)
                , _impl->write_buffers.begin());

            // TODO: Same as the comment in async_read_some operation
            _impl->write_impl([h = move(handler), impl = _impl]
                              (const system::error_code& ec, size_t size) {
                                 if (impl->closed()) {
                                    (*h)(asio::error::operation_aborted, 0);
                                 } else {
                                    (*h)(ec, size);
                                 }
                              });
        }
        else {
            asio::post(_executor, [h = move(handler)]
                                  { (*h)(asio::error::bad_descriptor, 0); });
        }

        return init.result.get();
    }

    const std::string& remote_endpoint() const { return _remote_endpoint; }

private:
#if BOOST_VERSION >= 107100
    asio::executor _executor;
#elif BOOST_VERSION >= 106700
    asio::io_service* _ios = nullptr;
#endif

    // Note: we must use shared_ptr because some stream implementations (such
    // as the asio::ssl::stream) require that their lifetime is preserved while
    // an async action is pending on them.
    std::shared_ptr<Base> _impl;
    std::string _remote_endpoint;
    bool _debug = false;
};

} // ouinet namespace
