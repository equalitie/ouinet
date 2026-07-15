#include "response_reader.h"
#include "util/async.h"
#include <boost/beast/http/read.hpp>

namespace ouinet::http_response {

Reader::Reader(GenericStream in)
    : _in(std::move(in))
    , _is_done(false)
{
    setup_parser();
}

GenericStream
Reader::release_stream()
{
    _parser.release();
    _on_chunk_header = nullptr;
    _on_chunk_body = nullptr;
    _next_part = std::nullopt;

    auto stream = std::move(_in);
    return stream;
}

void Reader::setup_parser()
{
    _on_chunk_header = [&] (auto size, auto exts, auto& ec) {
        assert(!_next_part);
        _next_part = ChunkHdr{size, exts.to_string()};
    };

    _on_chunk_body = [&] (auto remain, auto data, auto& ec) -> size_t {
        assert(!_next_part);
        _next_part = ChunkBody( std::vector<uint8_t>(data.begin(), data.end())
                              , remain - data.size());
        return data.size();
    };

    // Reads are both streamed and parts limited to `_buffer` size,
    // so remove the whole body size limit.
    _parser.body_limit((std::numeric_limits<std::size_t>::max)());
    // Increase the header size limit to 16KB to fix the loading of sites
    // with big headers.
    _parser.header_limit(16*1024);
    _parser.on_chunk_header(_on_chunk_header);
    _parser.on_chunk_body(_on_chunk_body);
}

std::expected<std::optional<Part>, sys::error_code>
Reader::async_read_part(Async yield) {
    if (_is_done) {
        return std::nullopt;
    }

    // Cancellation, time out and error handling
    auto lifetime_cancelled = _lifetime_cancel.connect([&] { yield.cancel(); });
    auto cancelled = yield.cancel_slot([&] { _in.close(); });

    // Receive HTTP response head from input side and parse it
    // -------------------------------------------------------
    if (!_parser.is_header_done()) {
        auto result = http::async_read_header(_in, _buffer, _parser, yield);
        if (!result) {
            return std::unexpected(result.error());
        }

        if (_parser.is_done() && !_is_done) {  // e.g. no body
            _is_done = true;
        }

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
        auto result = http::async_read_some(_in, _buffer, _parser, yield);

        if (!result) {
            assert(result.error() != http::error::end_of_stream);

            if (result.error() != http::error::end_of_chunk) {
                return std::unexpected(result.error());
            }
        }

        assert(_next_part);
        Part ret = std::move(*_next_part);
        _next_part = std::nullopt;

        return ret;
    } else {
        if (_parser.is_done() && !_is_done) {
            _is_done = true;
            return std::nullopt;
        }

        char buf[http_forward_block];

        _parser.get().body().data = buf;
        _parser.get().body().size = sizeof(buf);

        auto result = http::async_read_some(_in, _buffer, _parser, yield);

        if (!result) {
            assert(result.error() != http::error::end_of_stream);

            if (result.error() != http::error::need_buffer) {
                return std::unexpected(result.error());
            }
        }

        size_t s = sizeof(buf) - _parser.get().body().size;

        if (s == 0 && _parser.is_done()) {
            _is_done = true;
            return std::nullopt;
        }

        return Part(Body(std::vector<uint8_t>(buf, buf + s)));
    }
}

} // namespace ouinet
