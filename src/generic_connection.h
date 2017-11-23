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
protected:
    using OnRead  = std::function<void(sys::error_code, size_t)>;
    using OnWrite = std::function<void(sys::error_code, size_t)>;

    using ReadBuffers  = std::vector<asio::mutable_buffer>;
    using WriteBuffers = std::vector<asio::const_buffer>;

    template<class Token, class Ret>
    using Handler = typename asio::handler_type< Token
                                               , void(sys::error_code, Ret)
                                               >::type;

    template<class Token, class Ret>
    using Result = typename asio::async_result<Handler<Token, Ret>>;

public:
    virtual asio::io_service& get_io_service() = 0;

    virtual void read_impl (const ReadBuffers&,  OnRead&&)  = 0;
    virtual void write_impl(const WriteBuffers&, OnWrite&&) = 0;

    template< class MutableBufferSequence
            , class Token>
    typename Result<Token, size_t>::type
    async_read_some(const MutableBufferSequence& bs, Token&& token)
    {
        using namespace std;

        Handler<Token, size_t> handler(forward<Token>(token));
        Result<Token, size_t> result(handler);

        _read_buffers.resize(distance(bs.begin(), bs.end()));
        copy(bs.begin(), bs.end(), _read_buffers.begin());
        read_impl(_read_buffers, move(handler));

        return result.get();
    }

    template< class ConstBufferSequence
            , class Token>
    typename Result<Token, size_t>::type
    async_write_some(const ConstBufferSequence& bs, Token&& token)
    {
        using namespace std;

        Handler<Token, size_t> handler(forward<Token>(token));
        Result<Token, size_t> result(handler);

        _write_buffers.resize(distance(bs.begin(), bs.end()));
        copy(bs.begin(), bs.end(), _write_buffers.begin());
        write_impl(_write_buffers, move(handler));

        return result.get();
    }

    virtual ~GenericConnection() {}

private:
    ReadBuffers  _read_buffers;
    WriteBuffers _write_buffers;
};

template<class Impl>
class GenericConnectionImpl : public GenericConnection {
public:
    GenericConnectionImpl(Impl&& impl)
        : _impl(std::move(impl))
    {}

    virtual asio::io_service& get_io_service() override
    {
        return _impl.get_io_service();
    }

    Impl& get_impl() { return _impl; }

private:
    void read_impl(const ReadBuffers& bs, OnRead&& on_read) override
    {
        _impl.async_read_some(bs, std::move(on_read));
    }

    void write_impl(const WriteBuffers& bs, OnWrite&& on_write) override
    {
        _impl.async_write_some(bs, std::move(on_write));
    }

private:
    Impl _impl;
};

} // ouinet namespace
