#pragma once

#include "generic_stream.h"

namespace ouinet { namespace cache {

class Session {
public:
    Session(GenericStream con)
        : _con(std::move(con))
    {}

    http::response_header<>* response_header() {
        if (!_parser.is_header_done()) return nullptr;
        return &_parser.get().base();
    }

    http::response_header<>* read_response_header(Cancel&, asio::yield_context);

    template<class SinkStream>
    void flush_response(SinkStream&, Cancel&, asio::yield_context);

private:
    GenericStream _con;
    beast::static_buffer<16384> _buffer;
    http::response_parser<http::buffer_body> _parser;
};

inline
http::response_header<>*
Session::read_response_header(Cancel& cancel, asio::yield_context yield)
{
    if (auto h = response_header()) return h;

    auto c = cancel.connect([&] { _con.close(); });

    sys::error_code ec;
    http::async_read_header(_con, _buffer, _parser, yield[ec]);

    if (c) ec = asio::error::operation_aborted;
    if (ec) return or_throw<http::response_header<>*>(yield, ec, nullptr);

    assert(_parser.is_header_done());

    return response_header();
}

template<class SinkStream>
inline
void
Session::flush_response(SinkStream& sink,
                        Cancel& cancel,
                        asio::yield_context yield)
{
    // Used this as an example
    // https://www.boost.org/doc/libs/1_70_0/libs/beast/doc/html/beast/more_examples/http_relay.html

    auto c = cancel.connect([&] { _con.close(); });

    sys::error_code ec;

    http::response_serializer<http::buffer_body> sr{_parser.get()};
    
    read_response_header(cancel , yield[ec]); // Won't read if already read.
    return_or_throw_on_error(yield, cancel, ec);

    http::async_write_header(sink, sr, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec);

    char buf[2048];

    do {
        if (!_parser.is_done()) {
            _parser.get().body().data = buf;
            _parser.get().body().size = sizeof(buf);
            http::async_read(_con, _buffer, _parser, yield[ec]);

            if (ec == http::error::need_buffer) ec = {};
            return_or_throw_on_error(yield, cancel, ec);

            _parser.get().body().size = sizeof(buf) - _parser.get().body().size;
            _parser.get().body().data = buf;
            _parser.get().body().more = !_parser.is_done();
        } else {
            _parser.get().body().data = nullptr;
            _parser.get().body().size = 0;
        }

        http::async_write(sink, sr, yield[ec]);
        if (ec == http::error::need_buffer) ec = {};
        return_or_throw_on_error(yield, cancel, ec);
    }
    while (!_parser.is_done() && !sr.is_done());
}

}} // namespaces
