#pragma once

#include "generic_stream.h"

namespace ouinet {

class Session {
private:
    struct State {
        GenericStream con;
        beast::static_buffer<16384> buffer;
        http::response_parser<http::buffer_body> parser;

        State(GenericStream&& con) : con(std::move(con)) {}
    };

public:
    Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Session(Session&&) = default;
    Session& operator=(Session&&) = default;

    Session(GenericStream con)
        : _state(new State(std::move(con)))
    {}

    http::response_header<>* response_header() {
        if (!_state) return nullptr;
        if (!_state->parser.is_header_done()) return nullptr;
        return &_state->parser.get().base();
    }

    http::response_header<>* read_response_header(Cancel&, asio::yield_context);

    template<class SinkStream>
    void flush_response(SinkStream&, Cancel&, asio::yield_context);

    // Loads the entire response to memory, use only for debugging
    template<class BodyType>
    http::response<BodyType> slurp(Cancel&, asio::yield_context);

    void close() {
        if (!_state) return;
        if (_state->con.is_open()) _state->con.close();
    }

private:
    std::unique_ptr<State> _state;
};

inline
http::response_header<>*
Session::read_response_header(Cancel& cancel, asio::yield_context yield)
{
    if (!_state) {
        return or_throw<http::response_header<>*>(
                yield, asio::error::bad_descriptor);
    }

    if (auto h = response_header()) return h;

    auto c = cancel.connect([&] { _state->con.close(); });

    sys::error_code ec;
    http::async_read_header(_state->con,
                            _state->buffer,
                            _state->parser,
                            yield[ec]);

    if (c) ec = asio::error::operation_aborted;
    if (ec) return or_throw<http::response_header<>*>(yield, ec, nullptr);

    assert(_state->parser.is_header_done());

    return response_header();
}

template<class SinkStream>
inline
void
Session::flush_response(SinkStream& sink,
                        Cancel& cancel,
                        asio::yield_context yield)
{
    if (!_state) {
        return or_throw(yield, asio::error::bad_descriptor);
    }

    // Used this as an example
    // https://www.boost.org/doc/libs/1_70_0/libs/beast/doc/html/beast/more_examples/http_relay.html

    auto c = cancel.connect([&] { _state->con.close(); });

    sys::error_code ec;

    http::response_serializer<http::buffer_body> sr{_state->parser.get()};

    read_response_header(cancel , yield[ec]); // Won't read if already read.
    return_or_throw_on_error(yield, cancel, ec);

    http::async_write_header(sink, sr, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec);

    char buf[2048];

    do {
        if (!_state->parser.is_done()) {
            _state->parser.get().body().data = buf;
            _state->parser.get().body().size = sizeof(buf);
            http::async_read(_state->con, _state->buffer, _state->parser, yield[ec]);

            if (ec == http::error::need_buffer) ec = {};
            return_or_throw_on_error(yield, cancel, ec);

            _state->parser.get().body().size = sizeof(buf) - _state->parser.get().body().size;
            _state->parser.get().body().data = buf;
            _state->parser.get().body().more = !_state->parser.is_done();
        } else {
            _state->parser.get().body().data = nullptr;
            _state->parser.get().body().size = 0;
        }

        http::async_write(sink, sr, yield[ec]);
        if (ec == http::error::need_buffer) ec = {};
        return_or_throw_on_error(yield, cancel, ec);
    }
    while (!_state->parser.is_done() && !sr.is_done());
}

template<class BodyType>
http::response<BodyType> Session::slurp(Cancel& cancel, asio::yield_context yield)
{
    if (!_state) {
        return or_throw<http::response<BodyType>>(
                yield, asio::error::bad_descriptor);
    }

    auto c = cancel.connect([&] { _state->con.close(); });

    sys::error_code ec;

    http::response_serializer<http::buffer_body> sr{_state->parser.get()};

    read_response_header(cancel , yield[ec]); // Won't read if already read.
    return_or_throw_on_error(yield, cancel, ec, http::response<BodyType>{});

    http::response<BodyType> rs{*response_header()};

    char buf[2048];

    std::string s;
    while (!_state->parser.is_done() && !sr.is_done()) {
        _state->parser.get().body().data = buf;
        _state->parser.get().body().size = sizeof(buf);

        http::async_read(_state->con, _state->buffer, _state->parser, yield[ec]);

        if (ec == http::error::need_buffer) ec = {};
        return_or_throw_on_error(yield, cancel, ec, http::response<BodyType>{});

        size_t size = sizeof(buf) - _state->parser.get().body().size;
        s += std::string(buf, size);
    }

    typename BodyType::reader reader(rs, rs.body());
    reader.put(asio::buffer(s), ec);
    assert(!ec);

    return rs;
}

} // namespaces
