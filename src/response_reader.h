#pragma once

#include "generic_stream.h"
#include "namespaces.h"
#include "or_throw.h"
#include "response_part.h"
#include "util/signal.h"
#include <boost/beast.hpp>

namespace ouinet { namespace http_response {

class Reader {
private:
    static const size_t http_forward_block = 16384;
    using string_view = boost::string_view;

public:
    Reader(GenericStream in);

    //
    // Possible output on subsequent invocations per one response:
    //
    // Head >> (ChunkHdr(size > 0) ChunkBody*)* >> ChunkHdr(size == 0) >> Trailer >> boost::none*
    //
    // Or:
    //
    // Head >> Body* >> boost::none*
    //
    boost::optional<Part> async_read_part(Cancel, asio::yield_context);

    void restart()
    {
        // It is only valid to call restart() if we've finished reading
        // the whole response, or we haven't even started reading one.
        assert(!_parser.is_header_done() || _is_done || _parser.is_done());
        _is_done = false;
        (&_parser)->~parser();
        new (&_parser) (decltype(_parser))();
        set_callbacks();
    }

private:
    http::fields filter_trailer_fields(const http::fields& hdr)
    {
        http::fields trailer;
        for (const auto& field : http::token_list(hdr[http::field::trailer])) {
            auto i = hdr.find(field);
            if (i == hdr.end())
                continue;  // missing trailer
            trailer.insert(i->name(), i->name_string(), i->value());
        }
        return trailer;
    }

    void set_callbacks();

private:
    GenericStream _in;
    Cancel _lifetime_cancel;
    beast::static_buffer<http_forward_block> _buffer;
    http::response_parser<http::buffer_body> _parser;

    std::function<void(size_t, string_view, sys::error_code&)> _on_chunk_header;
    std::function<size_t(size_t, string_view, sys::error_code&)> _on_chunk_body;

    boost::optional<Part> _next_part;

    bool _is_done;
};

inline
Reader::Reader(GenericStream in)
    : _in(std::move(in))
    , _is_done(false)
{
    set_callbacks();
}

inline
void Reader::set_callbacks()
{
    _on_chunk_header = [&] (auto size, auto exts, auto& ec) {
        assert(!_next_part);
        _next_part = ChunkHdr{size, std::move(exts.to_string())};
    };

    _on_chunk_body = [&] (auto remain, auto data, auto& ec) -> size_t {
        assert(!_next_part);
        _next_part = ChunkBody( std::vector<uint8_t>(data.begin(), data.end())
                              , remain);
        return data.size();
    };

    _parser.on_chunk_header(_on_chunk_header);
    _parser.on_chunk_body(_on_chunk_body);
}

inline
boost::optional<Part>
Reader::async_read_part(Cancel cancel, asio::yield_context yield) {
    assert(!cancel);

    if (_is_done) {
        return boost::none;
    }

    // Cancellation, time out and error handling
    auto lifetime_cancelled = _lifetime_cancel.connect([&] { cancel(); });
    auto cancelled = cancel.connect([&] { _in.close(); });

    sys::error_code ec;

    // Receive HTTP response head from input side and parse it
    // -------------------------------------------------------
    if (!_parser.is_header_done()) {
        http::async_read_header(_in, _buffer, _parser, yield[ec]);

        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<Part>(yield, ec);

        return Part(Head(_parser.get().base()));
    }

    if (_parser.chunked()) {
        if (_parser.is_done() && !_is_done) {
            _is_done = true;
            auto hdr = _parser.release().base();
            return Part(Trailer{filter_trailer_fields(hdr)});
        }

        // Setting eager to false ensures that the callbacks shall be run only
        // once per each async_read_some call.
        _parser.eager(false);

        assert(!_next_part);
        http::async_read_some(_in, _buffer, _parser, yield[ec]);

        if (cancel) ec = asio::error::operation_aborted;
        if (ec == http::error::end_of_chunk) ec = {};
        if (ec) return or_throw(yield, ec, boost::none);

        assert(_next_part);
        Part ret = std::move(*_next_part);
        _next_part = boost::none;

        return ret;
    }
    else {
        if (_parser.is_done() && !_is_done) {
            _is_done = true;
            return boost::none;
        }

        char buf[http_forward_block];

        _parser.get().body().data = buf;
        _parser.get().body().size = sizeof(buf);

        http::async_read_some(_in, _buffer, _parser, yield[ec]);

        if (cancel) ec = asio::error::operation_aborted;
        if (ec == http::error::need_buffer) ec = sys::error_code();
        if (ec) return or_throw<Part>(yield, ec);

        size_t s = sizeof(buf) - _parser.get().body().size;

        if (s == 0 && _parser.is_done()) {
            _is_done = true;
            return boost::none;
        }

        return Part(Body(std::vector<uint8_t>(buf, buf + s)));
    }
}

}} // namespace ouinet::http_response
