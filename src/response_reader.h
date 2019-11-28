#pragma once

#include "generic_stream.h"
#include "namespaces.h"
#include "util/signal.h"
#include "util/yield.h"
#include <boost/beast.hpp>
#include <boost/variant.hpp>
#include <queue>

namespace ouinet {

class ResponseReader {
private:
    static const size_t http_forward_block = 16384;
    using string_view = boost::string_view;

public:
    struct Head : public http::response_header<>  {
        using Base = http::response_header<>;
        using Base::Base;
        Head(const Head&) = default;
        Head(Head&&) = default;
        Head& operator=(const Head&) = default;
        Head(const Base& b) : Base(b) {}
        Head(Base&& b) : Base(std::move(b)) {}
    };

    struct ChunkHdr {
        size_t size = 0; // Size of chunk body
        std::string exts;

        bool operator==(const ChunkHdr& other) const {
            return size == other.size && exts == other.exts;
        }
    };

    struct ChunkBody : public std::vector<uint8_t> {
        using Base = std::vector<uint8_t>;
        using Base::Base;
        ChunkBody(const ChunkBody&) = default;
        ChunkBody(ChunkBody&&) = default;
        ChunkBody& operator=(const ChunkBody&) = default;
        ChunkBody(const Base& b) : Base(b) {}
        ChunkBody(Base&& b) : Base(std::move(b)) {}
    };

    struct Body : public std::vector<uint8_t> {
        using Base = std::vector<uint8_t>;
        using Base::Base;
        Body(const Body&) = default;
        Body(Body&&) = default;
        Body& operator=(const Body&) = default;
        Body(const Base& b) : Base(b) {}
        Body(Base&& b) : Base(std::move(b)) {}
    };

    struct End {
        http::fields trailer;
    };

    using Part = boost::variant<Head, ChunkHdr, ChunkBody, Body, End>;

public:
    ResponseReader(GenericStream in);

    Part async_read_part(Cancel, Yield);

private:
    // https://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.4
    bool has_body() const {
        auto result = _parser.get().result();

        auto result_i
            = static_cast<std::underlying_type_t<http::status>>(result);

        bool inv = (100 <= result_i && result_i < 200)
                || result == http::status::no_content // 204
                || result == http::status::not_modified // 304
                /* TODO: Request method == HEAD */ ;

        return !inv;
    };

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
    }

private:
    GenericStream _in;
    Cancel _lifetime_cancel;
    beast::static_buffer<http_forward_block> _buffer;
    http::response_parser<http::buffer_body> _parser;

    std::function<void(size_t, string_view, sys::error_code&)> _on_chunk_header;
    std::function<size_t(size_t, string_view, sys::error_code&)> _on_chunk_body;

    ChunkHdr*  _chunk_hdr  = nullptr;
    ChunkBody* _chunk_body = nullptr;

    std::queue<Part> _queued_parts;
};

ResponseReader::ResponseReader(GenericStream in)
    : _in(std::move(in))
{
    _on_chunk_header = [&] (auto size, auto exts, auto& ec) {
        _queued_parts.push(ChunkHdr{size, std::move(exts.to_string())});
        _chunk_hdr = boost::get<ChunkHdr>(&_queued_parts.back());
    };

    _on_chunk_body = [&] (auto remain, auto data, auto& ec) -> size_t {
        if (!_chunk_body) {
            _queued_parts.push(ChunkBody(remain));
            _chunk_body = boost::get<ChunkBody>(&_queued_parts.back());
        }

        asio::mutable_buffer buf(asio::buffer(*_chunk_body));
        buf += buf.size() - remain;

        return asio::buffer_copy(buf, asio::buffer(data.data(), data.size()));
    };

    _parser.on_chunk_header(_on_chunk_header);
    _parser.on_chunk_body(_on_chunk_body);
}

ResponseReader::Part
ResponseReader::async_read_part(Cancel cancel, Yield yield_) {
    namespace Err = asio::error;

    if (!_queued_parts.empty()) {
        auto part = std::move(_queued_parts.front());
        _queued_parts.pop();
        return part;
    }

    Yield yield = yield_.tag("http_forward");

    // Cancellation, time out and error handling
    auto lifetime_cancelled = _lifetime_cancel.connect([&] { cancel(); });
    auto cancelled = cancel.connect([&] { _in.close(); });

    sys::error_code ec;

    auto set_error = [&] (sys::error_code& ec, const auto& msg) {
        if (cancelled) ec = Err::operation_aborted;
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
        if (_parser.is_done()) {
            auto hdr = _parser.release().base();
            reset_parser();
            return End{filter_trailer_fields(hdr)};
        }

        while (true) {
            http::async_read(_in, _buffer, _parser, yield[ec]);

            if (ec == http::error::end_of_chunk) {
                assert(!_queued_parts.empty());
                auto ret = _queued_parts.front();
                _queued_parts.pop();
                _chunk_hdr  = nullptr;
                _chunk_body = nullptr;
                return ret;
            }

            if (set_error(ec, "Failed to parse chunk")) {
                return or_throw<Part>(yield, ec);
            }

            if (_queued_parts.empty()) continue;

            auto hdr = boost::get<ChunkHdr>(&_queued_parts.front());

            if (hdr) {
                auto ret = std::move(*hdr);
                _queued_parts.pop();
                _chunk_hdr = nullptr;
                return ret;
            }
        }
    } else {
        auto opt_len = _parser.content_length();
        assert(opt_len && "TODO");
        std::vector<uint8_t> body(*opt_len);
        asio::mutable_buffer buf(body.data(), body.size());
        size_t s = asio::buffer_copy(buf, _buffer.data());
        _buffer.consume(s);
        buf += s;

        while (buf.size()) {
            buf += _in.async_read_some(buf, yield[ec]);
            if (ec) return or_throw<Part>(yield, ec);
        }

        reset_parser();

        return Body(move(body));
    }

    return Part();
}

} // namespace ouinet
