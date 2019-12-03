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

    struct Body : public std::vector<uint8_t> {
        using Base = std::vector<uint8_t>;
        using Base::Base;
        Body(const Body&) = default;
        Body(Body&&) = default;
        Body& operator=(const Body&) = default;
        Body(const Base& b) : Base(b) {}
        Body(Base&& b) : Base(std::move(b)) {}
    };

    struct ChunkHdr {
        size_t size; // Size of chunk body
        std::string exts;

        ChunkHdr() : size(0) {}
        ChunkHdr(size_t size, std::string exts)
            : size(size)
            , exts(std::move(exts))
        {}

        bool operator==(const ChunkHdr& other) const {
            return size == other.size && exts == other.exts;
        }
    };

    struct ChunkBody : public std::vector<uint8_t> {
        size_t remain;

        using Base = std::vector<uint8_t>;

        ChunkBody(const Base& data, size_t remain)
            : Base(data)
            , remain(remain) {}

        ChunkBody(const ChunkBody&) = default;
        ChunkBody(ChunkBody&&) = default;
        ChunkBody& operator=(const ChunkBody&) = default;
    };

    struct Trailer : public http::fields {
        using Base = http::fields;
        using Base::Base;
        Trailer(const Trailer&) = default;
        Trailer(Trailer&&) = default;
        Trailer& operator=(const Trailer&) = default;
        Trailer(const Base& b) : Base(b) {}
        Trailer(Base&& b) : Base(std::move(b)) {}
    };

private:
    using PartVariant = boost::variant<Head, ChunkHdr, ChunkBody, Body, Trailer>;

public:
    struct Part : public PartVariant
    {
        using Base = PartVariant;
        using Base::Base;
        Part() = default;
        Part(const Part&) = default;
        Part(Part&&) = default;
        Part& operator=(const Part&) = default;
        Part(const Base& b) : Base(b) {}
        Part(Base&& b) : Base(std::move(b)) {}

        bool operator==(const Part& that) const {
            return static_cast<const Base&>(*this)
                == static_cast<const Base&>(that);
        }

        Head*      as_head()       { return boost::get<Head>     (this); }
        Body*      as_body()       { return boost::get<Body>     (this); }
        ChunkHdr*  as_chunk_hdr()  { return boost::get<ChunkHdr> (this); }
        ChunkBody* as_chunk_body() { return boost::get<ChunkBody>(this); }
        Trailer*   as_trailer()    { return boost::get<Trailer>  (this); }

        const Head*      as_head()       const { return boost::get<Head>     (this); }
        const Body*      as_body()       const { return boost::get<Body>     (this); }
        const ChunkHdr*  as_chunk_hdr()  const { return boost::get<ChunkHdr> (this); }
        const ChunkBody* as_chunk_body() const { return boost::get<ChunkBody>(this); }
        const Trailer*   as_trailer()    const { return boost::get<Trailer>  (this); }

        bool is_head()       const { return as_head()       != nullptr; }
        bool is_body()       const { return as_body()       != nullptr; }
        bool is_chunk_hdr()  const { return as_chunk_hdr()  != nullptr; }
        bool is_chunk_body() const { return as_chunk_body() != nullptr; }
        bool is_trailer()    const { return as_trailer()    != nullptr; }
    };

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

    std::queue<Part> _queued_parts;
};

ResponseReader::ResponseReader(GenericStream in)
    : _in(std::move(in))
{
    set_callbacks();
}

inline
void ResponseReader::set_callbacks()
{
    _on_chunk_header = [&] (auto size, auto exts, auto& ec) {
        _queued_parts.push(ChunkHdr{size, std::move(exts.to_string())});
    };

    _on_chunk_body = [&] (auto remain, auto data, auto& ec) -> size_t {
        _queued_parts.push(ChunkBody( std::vector<uint8_t>( data.begin()
                                                          , data.end())
                                    , remain));
        return data.size();
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
            return Trailer{filter_trailer_fields(hdr)};
        }

        while (_queued_parts.empty()) {
            http::async_read(_in, _buffer, _parser, yield[ec]);

            if (ec == http::error::end_of_chunk) {
                ec = {};
            }

            if (set_error(ec, "Failed to parse chunk")) {
                return or_throw<Part>(yield, ec);
            }
        }

        auto part = std::move(_queued_parts.front());
        _queued_parts.pop();
        return part;
    }
    else {
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
