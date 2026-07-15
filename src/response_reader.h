#pragma once

#include <limits>

#include "generic_stream.h"
#include "response_part.h"
#include "util/cancel.h"
#include "namespaces.h"
#include "util/select.h"
#include "api.h"

#include <boost/beast/core/static_buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/parser.hpp>

namespace ouinet {
    class Async;
}

namespace ouinet::http_response {

class AbstractReader {
public:
    virtual std::expected<std::optional<Part>, sys::error_code> async_read_part(Async) = 0;

    virtual bool is_done() const = 0;

    virtual void close()   = 0;

    virtual asio::any_io_executor get_executor() = 0;

    virtual ~AbstractReader() = default;

    template<class Duration>
    std::expected<std::optional<Part>, sys::error_code>
    timed_async_read_part(Duration d, Async yield)
    {
        return timeout(d, [&](Async yield) { return async_read_part(yield); }, yield);
    }
};

class OUINET_COMMON_API Reader : public AbstractReader {
private:
    static const size_t http_forward_block = 16384;
    using string_view = boost::string_view;

public:
    Reader(GenericStream in);
    virtual ~Reader() = default;

    //
    // Possible output on subsequent invocations per one response:
    //
    // Head >> ( ChunkHdr(size > 0) >> ChunkBody(remain > 0)* >> ChunkBody(remain == 0) )*
    //      >> ChunkHdr(size == 0) >> Trailer >> boost::none*
    //
    // Or:
    //
    // Head >> Body* >> boost::none*
    //
    std::expected<std::optional<Part>, sys::error_code> async_read_part(Async) override;

    bool is_done() const override { return _is_done; }

    // This leaves the reader in an undefined state,
    // do not use afterwards.
    GenericStream release_stream();

    GenericStream& stream() { return _in; }

    void restart()
    {
        // It is only valid to call restart() if we've finished reading
        // the whole response, or we haven't even started reading one.
        assert(!_parser.is_header_done() || _is_done || _parser.is_done());
        _is_done = false;
        (&_parser)->~parser();
        new (&_parser) (decltype(_parser))();
        setup_parser();
    }

    void close() override { if (_in.is_open()) _in.close(); }

    asio::any_io_executor get_executor() override { return _in.get_executor(); }

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

    void setup_parser();

private:
    GenericStream _in;
    Cancel _lifetime_cancel;
    beast::static_buffer<http_forward_block> _buffer;
    http::response_parser<http::buffer_body> _parser;

    std::function<void(size_t, string_view, sys::error_code&)> _on_chunk_header;
    std::function<size_t(size_t, string_view, sys::error_code&)> _on_chunk_body;

    std::optional<Part> _next_part;

    bool _is_done;
};

} // namespace ouinet::http_response
