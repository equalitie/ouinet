#pragma once

#include "generic_stream.h"
#include "namespaces.h"
#include "util/signal.h"
#include "util/yield.h"
#include <boost/beast.hpp>
#include <boost/variant.hpp>
#include <queue>

namespace ouinet {

namespace Http {
    namespace Rsp {
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

        struct Body      : public std::vector<uint8_t> {
            using Base = std::vector<uint8_t>;
            using Base::Base;
            Body(const Body&) = default;
            Body(Body&&) = default;
            Body& operator=(const Body&) = default;
            Body(const Base& b) : Base(b) {}
            Body(Base&& b) : Base(std::move(b)) {}
        };

        struct Trailer   : public http::fields {
            using http::fields::fields;
        };

        using Part = boost::variant<Head, ChunkHdr, ChunkBody, Body, Trailer>;
    }

} // Http namespace

class ResponseReader {
private:
    static const size_t http_forward_block = 16384;
    using string_view = boost::string_view;

public:
    using Part = Http::Rsp::Part;

public:
    ResponseReader(GenericStream in);

    Http::Rsp::Part async_read_part(Cancel, Yield);

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

private:
    GenericStream _in;
    Cancel _lifetime_cancel;
    beast::static_buffer<http_forward_block> _buffer;
    http::response_parser<http::buffer_body> _parser;

    std::function<void(size_t, string_view, sys::error_code&)> _on_chunk_header;
    std::function<size_t(size_t, string_view, sys::error_code&)> _on_chunk_body;

    Http::Rsp::ChunkHdr*  _chunk_hdr  = nullptr;
    Http::Rsp::ChunkBody* _chunk_body = nullptr;

    std::queue<Http::Rsp::Part> _queued_parts;
};

ResponseReader::ResponseReader(GenericStream in)
    : _in(std::move(in))
{
    _on_chunk_header = [&] (auto size, auto exts, auto& ec) {
        using Hdr  = Http::Rsp::ChunkHdr;

        _queued_parts.push(Hdr{size, std::move(exts.to_string())});
        _chunk_hdr = boost::get<Hdr>(&_queued_parts.back());
    };

    _on_chunk_body = [&] (auto remain, auto data, auto& ec) -> size_t {
        using Body = Http::Rsp::ChunkBody;

        if (!_chunk_body) {
            _queued_parts.push(Body(remain));
            _chunk_body = boost::get<Body>(&_queued_parts.back());
        }

        asio::mutable_buffer buf(asio::buffer(*_chunk_body));
        buf += buf.size() - remain;

        return asio::buffer_copy(buf, asio::buffer(data.data(), data.size()));
    };

    _parser.on_chunk_header(_on_chunk_header);
    _parser.on_chunk_body(_on_chunk_body);
}

Http::Rsp::Part ResponseReader::async_read_part(Cancel cancel, Yield yield_) {
    namespace Rsp = Http::Rsp;
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
        if (set_error(ec, "Failed to receive response head"))
            return or_throw<Rsp::Part>(yield, ec);
        return Rsp::Head(_parser.get().base());
    }

    assert(_parser.is_header_done());

    if (_parser.chunked()) {
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
                return or_throw<Rsp::Part>(yield, ec);
            }

            if (_queued_parts.empty()) continue;

            auto hdr = boost::get<Rsp::ChunkHdr>(&_queued_parts.front());

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
        buf += asio::buffer_copy(buf, _buffer.data());
        while (buf.size()) {
            buf += _in.async_read_some(buf, yield[ec]);
        }
        return Rsp::Body(move(body));
    }

    return Http::Rsp::Part();
}

} // namespace ouinet
