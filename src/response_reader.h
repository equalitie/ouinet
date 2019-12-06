#pragma once

#include "generic_stream.h"
#include "namespaces.h"
#include "or_throw.h"
#include "response_part.h"
#include "util/signal.h"
#include "util/yield.h"
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
    // Head >> (ChunkHdr(size > 0) ChunkBody*)* >> ChunkHdr(size == 0) >> Trailer
    //
    // Or:
    //
    // Head >> Body(is_last == false)* >> Body(is_last == true)
    //
    Part async_read_part(Cancel, Yield);

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

    void reset_parser()
    {
        (&_parser)->~parser();
        new (&_parser) (decltype(_parser))();

        set_callbacks();
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
};

inline
Reader::Reader(GenericStream in)
    : _in(std::move(in))
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
Part
Reader::async_read_part(Cancel cancel, Yield yield_) {
    Yield yield = yield_.tag("http_forward");

    if (_parser.is_done() && !_parser.chunked()) {
        bool need_eof = _parser.need_eof();
        reset_parser();
        if (need_eof) {
            return or_throw<Part>(yield, http::error::end_of_stream);
        }
    }

    // Cancellation, time out and error handling
    auto lifetime_cancelled = _lifetime_cancel.connect([&] { cancel(); });
    auto cancelled = cancel.connect([&] { _in.close(); });

    sys::error_code ec;

    auto set_error = [&] (sys::error_code& ec, const auto& msg) {
        if (cancelled) ec = asio::error::operation_aborted;
        if (ec) yield.log(msg, ": ", ec.message());
        return ec;
    };

    // Receive HTTP response head from input side and parse it
    // -------------------------------------------------------
    if (!_parser.is_header_done()) {
        http::async_read_header(_in, _buffer, _parser, yield[ec]);
        if (ec == http::error::end_of_stream) {
            return or_throw<Part>(yield, ec);
        }

        if (set_error(ec, "Failed to receive response head"))
            return or_throw<Part>(yield, ec);

        return Head(_parser.get().base());
    }

    if (_parser.chunked()) {
        // Setting eager to false ensures that the callbacks shall be run only
        // once per each async_read_some call.
        _parser.eager(false);

        if (_parser.is_done()) {
            auto hdr = _parser.release().base();
            reset_parser();
            return Trailer{filter_trailer_fields(hdr)};
        }

        assert(!_next_part);
        http::async_read_some(_in, _buffer, _parser, yield[ec]);

        if (ec == http::error::end_of_chunk) {
            ec = {};
        }

        if (set_error(ec, "Failed to parse chunk")) {
            return or_throw<Part>(yield, ec);
        }

        assert(_next_part);
        Part ret = std::move(*_next_part);
        _next_part = boost::none;
        return ret;
    }
    else {
        char buf[http_forward_block];

        _parser.get().body().data = buf;
        _parser.get().body().size = sizeof(buf);

        if (_parser.need_eof() && _parser.is_done()) {
            reset_parser();
            return or_throw<Part>(yield, http::error::end_of_stream);
        }

        http::async_read_some(_in, _buffer, _parser, yield[ec]);

        size_t s = sizeof(buf) - _parser.get().body().size;

        if (ec == http::error::need_buffer) ec = sys::error_code();
        if (ec) return or_throw<Part>(yield, ec);

        if (_parser.need_eof() && _parser.is_done()) {
            reset_parser();
            return or_throw<Part>(yield, http::error::end_of_stream);
        }

        return Body(_parser.is_done(), std::vector<uint8_t>(buf, buf + s));
    }
}

}} // namespace ouinet::http_response
