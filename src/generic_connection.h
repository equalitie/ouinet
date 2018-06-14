#pragma once

#include "namespaces.h"

#include <boost/system/error_code.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/spawn.hpp>
#include <functional>
#include <vector>
#include <iostream>

namespace ouinet {

class GenericConnection {
private:
    template<class Token, class Ret>
    using Handler = typename asio::handler_type< Token
                                               , void(sys::error_code, Ret)
                                               >::type;

    template<class Token, class Ret>
    using Result = typename asio::async_result<Handler<Token, Ret>>;

    using OnRead  = std::function<void(sys::error_code, size_t)>;
    using OnWrite = std::function<void(sys::error_code, size_t)>;

    using ReadBuffers  = std::vector<asio::mutable_buffer>;
    using WriteBuffers = std::vector<asio::const_buffer>;

    using executor_type = asio::io_context::executor_type;

    struct Base {
        virtual asio::io_service& get_io_service() = 0;
        virtual executor_type     get_executor() = 0;

        virtual void read_impl (OnRead&&)  = 0;
        virtual void write_impl(OnWrite&&) = 0;

        virtual void close() = 0;

        virtual ~Base() {}

        ReadBuffers  read_buffers;
        WriteBuffers write_buffers;
    };

    template<class Impl>
    struct Wrapper : public Base {
        using Shutter = std::function<void(Impl&)>;

        Wrapper(Impl&& impl)
            : _impl(std::move(impl))
            , _shutter([](Impl& impl) { impl.close(); })
        {}

        Wrapper(Impl&& impl, Shutter shutter)
            : _impl(std::move(impl))
            , _shutter(std::move(shutter))
        {}

        virtual asio::io_service& get_io_service() override
        {
            return _impl.get_io_service();
        }

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
            _shutter(_impl);
        }

    private:
        Impl _impl;
        Shutter _shutter;
    };

public:
    GenericConnection() {}

    template<class AsyncRWStream>
    GenericConnection(AsyncRWStream&& impl)
        : _impl(new Wrapper<AsyncRWStream>(std::forward<AsyncRWStream>(impl)))
    {}

    template<class AsyncRWStream, class Shutter>
    GenericConnection( AsyncRWStream&& impl
                     , Shutter shutter)
        : _impl(new Wrapper<AsyncRWStream>( std::forward<AsyncRWStream>(impl)
                                          , std::move(shutter)))
    {}

    asio::io_service& get_io_service()
    {
        return _impl->get_io_service();
    }

    executor_type get_executor()
    {
        return _impl->get_executor();
    }

    void close()
    {
        _impl->close();
    }

    template< class MutableBufferSequence
            , class Token>
    typename Result<Token, size_t>::type
    async_read_some(const MutableBufferSequence& bs, Token&& token)
    {
        using namespace std;

        namespace asio   = boost::asio;
        namespace system = boost::system;

        using Sig     = void(system::error_code, size_t);
        using Result  = asio::async_result<Token, Sig>;
        using Handler = typename Result::completion_handler_type;

        // XXX: Handler is non-copyable, but can we do this without allocation?
        auto handler = make_shared<Handler>(forward<decltype(token)>(token));

        Result result(*handler);

        _impl->read_buffers.resize(distance( asio::buffer_sequence_begin(bs)
                                           , asio::buffer_sequence_end(bs)));

        copy( asio::buffer_sequence_begin(bs)
            , asio::buffer_sequence_end(bs)
            , _impl->read_buffers.begin());

        _impl->read_impl([h = move(handler)]
                         (const system::error_code& ec, size_t size) {
                             (*h)(ec, size);
                         });

        return result.get();
    }

    template< class ConstBufferSequence
            , class Token>
    typename Result<Token, size_t>::type
    async_write_some(const ConstBufferSequence& bs, Token&& token)
    {
        using namespace std;

        namespace asio   = boost::asio;
        namespace system = boost::system;

        using Sig     = void(system::error_code, size_t);
        using Result  = asio::async_result<Token, Sig>;
        using Handler = typename Result::completion_handler_type;

        // XXX: Handler is non-copyable, but can we do this without allocation?
        auto handler = make_shared<Handler>(forward<decltype(token)>(token));

        Result result(*handler);

        _impl->write_buffers.resize(distance( asio::buffer_sequence_begin(bs)
                                            , asio::buffer_sequence_end(bs)));

        copy( asio::buffer_sequence_begin(bs)
            , asio::buffer_sequence_end(bs)
            , _impl->write_buffers.begin());

        _impl->write_impl([h = move(handler)]
                          (const system::error_code& ec, size_t size) {
                              (*h)(ec, size);
                          });

        return result.get();
    }

private:
    std::unique_ptr<Base> _impl;
};

} // ouinet namespace
