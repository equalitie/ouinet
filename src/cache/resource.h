#pragma once

#include <boost/optional.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include "../util/file_io.h"
#include "../util/hash.h"
#include "../util.h"
#include "../parse/number.h"
#include "../response_reader.h"
#include "../http_util.h"
#include "http_sign.h"
#include "signed_head.h"
#include "logger.h"
#include <boost/format.hpp>

#define CACHE_RESOURCE_LOGPFX "Cache resource: "
#define CACHE_RESOURCE_DEBUG(...) LOG_DEBUG(CACHE_RESOURCE_LOGPFX, __VA_ARGS__)
#define CACHE_RESOURCE_WARN(...) LOG_WARN(CACHE_RESOURCE_LOGPFX, __VA_ARGS__)
#define CACHE_RESOURCE_ERROR(...) LOG_ERROR(CACHE_RESOURCE_LOGPFX, __VA_ARGS__)

namespace ouinet::cache {

// TODO: There is already `util::Http{Request,Response}ByteRange`
struct Range {
    std::size_t begin, end;
};

// A signatures file entry with `OFFSET[i] SIGNATURE[i] BLOCK_DIGEST[i] CHASH[i-1]`.
// TODO: implement `ouipsig`
struct SigEntry {
    std::size_t offset;
    std::string signature;
    std::string block_digest;
    std::string prev_chained_digest;

    using parse_buffer = std::string;

    static const std::string& pad_digest() {
        static const auto pad_digest = util::base64_encode(util::SHA512::zero_digest());
        return pad_digest;
    }

    std::string str() const
    {
        static const auto line_format = "%016x %s %s %s\n";
        return ( boost::format(line_format) % offset % signature % block_digest
               % (prev_chained_digest.empty() ? pad_digest() : prev_chained_digest)).str();
    }

    std::string chunk_exts() const
    {
        std::ostringstream exts;

        static const auto fmt_sx = ";" + http_::response_block_signature_ext + "=\"%s\"";
        if (!signature.empty())
            exts << (boost::format(fmt_sx) % signature);

        static const auto fmt_hx = ";" + http_::response_block_chain_hash_ext + "=\"%s\"";
        if (!prev_chained_digest.empty())
            exts << (boost::format(fmt_hx) % prev_chained_digest);

        return exts.str();
    }

    template<class Stream>
    [[nodiscard]]
    static
    std::expected<boost::optional<SigEntry>, sys::error_code>
    parse(Stream& in, parse_buffer& buf, Async yield)
    {
        // Note: using asio_yield here because I'm not sure whether the `eof` error code may
        // happen while still receiving data.
        sys::error_code ec;
        auto line_len = asio::async_read_until(in, asio::dynamic_buffer(buf), '\n', yield.asio_yield()[ec]);
        if (yield.is_cancelled()) throw Async::Cancelled();
        if (ec == asio::error::eof) ec = {};
        if (ec) return std::unexpected(ec);

        if (line_len == 0) return boost::none;
        assert(line_len <= buf.size());
        if (buf[line_len - 1] != '\n') {
            CACHE_RESOURCE_ERROR("Truncated signature line");
            return std::unexpected(sys::errc::make_error_code(sys::errc::bad_message));
        }
        boost::string_view line(buf);
        line.remove_suffix(buf.size() - line_len + 1);  // leave newline out

        static const boost::regex line_regex(  // Ensure lines are fixed size!
            "([0-9a-f]{16})"  // PAD016_LHEX(OFFSET[i])
            " ([A-Za-z0-9+/=]{88})"  // BASE64(SIG[i]) (88 = size(BASE64(Ed25519-SIG)))
            " ([A-Za-z0-9+/=]{88})"  // BASE64(DHASH[i]) (88 = size(BASE64(SHA2-512)))
            " ([A-Za-z0-9+/=]{88})"  // BASE64(CHASH([i-1])) (88 = size(BASE64(SHA2-512)))
        );
        boost::cmatch m;
        if (!boost::regex_match(line.begin(), line.end(), m, line_regex)) {
            CACHE_RESOURCE_ERROR("Malformed signature line");
            return std::unexpected(sys::errc::make_error_code(sys::errc::bad_message));
        }
        auto offset = parse_data_block_offset(m[1].str());
        SigEntry entry{ offset, m[2].str(), m[3].str()
                      , (m[4] == pad_digest() ? "" : m[4].str())};
        buf.erase(0, line_len);  // consume used input
        return entry;
    }

private:
    static
    std::size_t
    parse_data_block_offset(const std::string& s)  // `^[0-9a-f]*$`
    {
        std::size_t offset = 0;
        for (auto& c : s) {
            assert(('0' <= c && c <= '9') || ('a' <= c && c <= 'f'));
            offset <<= 4;
            offset += ('0' <= c && c <= '9') ? c - '0' : c - 'a' + 10;
        }
        return offset;
    }
};

template<class File>
class GenericResourceReader : public http_response::AbstractReader {
private:
    static const std::size_t http_forward_block = 16384;

public:
    template<class IStream>
    [[nodiscard]]
    static
    std::expected<SignedHead, sys::error_code> read_signed_head(IStream& is, Async yield) {
        assert(is.is_open());

        auto on_cancel = yield.cancel_slot([&] { is.close(); });

        // Put in heap to avoid exceeding coroutine stack limit.
        auto buffer = std::make_unique<beast::static_buffer<http_forward_block>>();
        auto parser = std::make_unique<http::response_parser<http::empty_body>>();

        if (auto r = http::async_read_header(is, *buffer, *parser, yield); !r) {
            return std::unexpected(r.error());
        }

        if (!parser->is_header_done()) {
            return std::unexpected(sys::errc::make_error_code(sys::errc::no_message));
        }

        auto head_o = SignedHead::create_from_trusted_source(parser->release().base());

        if (!head_o) {
            return std::unexpected(sys::errc::make_error_code(sys::errc::no_message));
        }

        return std::move(*head_o);
    }

private:
    [[nodiscard]]
    std::expected<http_response::Head, sys::error_code>
    prepare_head()
    {
        uri = std::string(head[http_::response_uri_hdr]);
        if (uri.empty()) {
            CACHE_RESOURCE_ERROR("Missing URI in stored head");
            return std::unexpected(asio::error::bad_descriptor);
        }

        block_size = head.block_size();
        auto data_size_hdr = head[http_::response_data_size_hdr];
        auto data_size_opt = parse::number<std::size_t>(data_size_hdr);
        if (!data_size_opt)
            CACHE_RESOURCE_WARN("Loading incomplete stored response; uri=", uri);
        else
            data_size = *data_size_opt;

        // Create a partial content response if a range was specified.
        if (range) {
            auto orig_status = head.result_int();
            head.reason("");
            head.result(http::status::partial_content);
            head.set(http_::response_original_http_status, std::to_string(orig_status));

            // Align ranges to data blocks.
            assert(block_size);
            auto bs = *block_size;
            range->begin = bs * (range->begin / bs);  // align down
            range->end = range->end > 0  // align up
                       ? bs * ((range->end - 1) / bs + 1)
                       : 0;
            // Clip range end to actual file size.
            if (bodyf.is_open()) {
                auto ds = util::file_io::file_size(bodyf);
                if (!ds) return std::unexpected(ds.error());
                if (range->end > *ds) range->end = *ds;
            }
            else {
                range->end = 0;
            }
            //size_t ds = 0;
            //if (bodyf.is_open()) ds = util::file_io::file_size(bodyf, ec);
            //if (ec) return {};
            //if (range->end > ds) range->end = ds;

            // Report resulting range.
            std::stringstream content_range_ss;
            content_range_ss << util::HttpResponseByteRange{range->begin, range->end - 1, data_size};
            head.set( http::field::content_range, content_range_ss.str());
        }

        // The stored head should not have framing headers,
        // check and enable chunked transfer encoding.
        if (!( head[http::field::content_length].empty()
             && head[http::field::transfer_encoding].empty()
             && head[http::field::trailer].empty())) {
            CACHE_RESOURCE_WARN("Found framing headers in stored head, cleaning; uri=", uri);
            auto retval = http_injection_merge(std::move(head), {});
            retval.set(http::field::transfer_encoding, "chunked");
            return retval;
        }

        head.set(http::field::transfer_encoding, "chunked");
        return std::move(head);
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    seek_to_range_begin(Async yield)
    {
        assert(_is_head_done);
        if (!range) return {};
        if (range->end == 0) return {};
        assert(bodyf.is_open());
        assert(block_size);

        // Move body file pointer to start of range.
        block_offset = range->begin;
        if (auto r = util::file_io::fseek(bodyf, block_offset); !r) {
            return std::unexpected(r.error());
        }

        // Consume signatures before the first block.
        for (unsigned b = 0; b < (block_offset / *block_size); ++b) {
            if (auto r = get_sig_entry(yield); !r) {
                return std::unexpected(r.error());
            }
        }

        return {};
    }

protected:
    [[nodiscard]]
    std::expected<boost::optional<SigEntry>, sys::error_code>
    get_sig_entry(Async yield)
    {
        assert(_is_head_done);
        if (!sigsf.is_open()) return boost::none;

        return SigEntry::parse(sigsf, sigs_buffer, yield);
    }

private:
    [[nodiscard]]
    std::expected<http_response::ChunkBody, sys::error_code>
    get_chunk_body(Async yield)
    {
        assert(_is_head_done);
        http_response::ChunkBody empty_cb{{}, 0};

        if (!bodyf.is_open()) return empty_cb;

        if (body_buffer.size() == 0) {
            assert(block_size);
            body_buffer.resize(*block_size);
        }

        sys::error_code ec;
        auto len = asio::async_read(bodyf, asio::buffer(body_buffer), yield.asio_yield()[ec]);
        if (yield.is_cancelled()) throw Async::Cancelled();
        if (ec == asio::error::eof) ec = {};
        if (ec) return std::unexpected(ec);

        assert(len <= body_buffer.size());
        return http_response::ChunkBody{std::vector<uint8_t>(body_buffer.cbegin(), body_buffer.cbegin() + len), 0};
    }

    [[nodiscard]]
    std::expected<std::optional<http_response::Part>, sys::error_code>
    get_chunk_part(Async yield)
    {
        if (next_chunk_body) {
            // We just sent a chunk header, body comes next.
            auto part = std::move(next_chunk_body);
            next_chunk_body = std::nullopt;
            return part;
        }

        // Get block signature and previous hash,
        // and then its data (which may be empty).
        auto sig_entry_r = get_sig_entry(yield);
        if (!sig_entry_r) return std::unexpected(sig_entry_r.error());
        auto sig_entry = std::move(*sig_entry_r);

        // Even if there is no new signature entry,
        // if the signature of the previous block was read
        // it may still be worth sending it in this chunk header
        // (to allow the receiving end to process it).
        // Otherwise it is not worth sending anything.
        if (!sig_entry && next_chunk_exts.empty()) {
            if (!data_size) return std::unexpected(asio::error::connection_aborted);  // incomplete
            return std::nullopt;
        }
        auto chunk_body = get_chunk_body(yield);
        if (!chunk_body) return std::unexpected(chunk_body.error());

        // Validate block offset and size.
        if (sig_entry && sig_entry->offset != block_offset) {
            CACHE_RESOURCE_ERROR("Data block offset mismatch: ", sig_entry->offset, " != ", block_offset);
            return std::unexpected(make_error_code(sys::errc::bad_message));
        }
        block_offset += chunk_body->size();

        if (range && block_offset >= range->end) {
            // Hit range end, stop getting more blocks:
            // the next read data block will be empty,
            // thus generating a "last chunk" below.
            sigsf.close();
            bodyf.close();
        }

        if (chunk_body->size() == 0 && next_chunk_exts.empty() && sig_entry)
            // Empty body, generate last chunk header with the signature we just read.
            return http_response::Part(http_response::ChunkHdr(0, sig_entry->chunk_exts()));

        http_response::ChunkHdr ch(chunk_body->size(), next_chunk_exts);
        next_chunk_exts = sig_entry ? sig_entry->chunk_exts() : "";
        if (sig_entry && chunk_body->size() > 0)
            next_chunk_body = std::move(*chunk_body);
        return http_response::Part(std::move(ch));
    }

public:
    GenericResourceReader( SignedHead head
                         , File sigsf
                         , File bodyf
                         , std::optional<Range> range)
        : head(std::move(head))
        , sigsf(std::move(sigsf))
        , bodyf(std::move(bodyf))
        , range(range)
    {}

    ~GenericResourceReader() override {};

    [[nodiscard]]
    std::expected<std::optional<ouinet::http_response::Part>, sys::error_code>
    async_read_part(Async yield) override
    {
        if (!_is_open || _is_done) return std::nullopt;

        if (!_is_head_done) {
            auto head = prepare_head();
            if (!head) {
                return std::unexpected(head.error());
            }

            _is_head_done = true;

            if (auto r = seek_to_range_begin(yield); !r) {
                return std::unexpected(r.error());
            }

            return http_response::Part(std::move(*head));
        }

        if (!_is_body_done) {
            auto chunk_part = get_chunk_part(yield);
            if (!chunk_part) {
                return std::unexpected(chunk_part.error());
            }
            if (!*chunk_part) {
                return std::nullopt;
            }
            if (auto ch = (**chunk_part).as_chunk_hdr()) {
                _is_body_done = (ch->size == 0);  // last chunk
            }

            return chunk_part;
        }

        _is_done = true;
        close();
        return http_response::Part(http_response::Trailer());
    }

    bool is_done() const override
    {
        return _is_done;
    }

    AsioExecutor get_executor() override
    {
        return sigsf.get_executor();
    }

    bool
    is_open() const
    {
        return _is_open;
    }

    void
    close() override
    {
        _is_open = false;
        sigsf.close();
        bodyf.close();
    }

protected:
    SignedHead head;
    File sigsf;
    File bodyf;

    std::optional<Range> range;

    std::string uri;  // for warnings
    std::optional<std::size_t> data_size;
    std::optional<std::size_t> block_size;

private:
    bool _is_head_done = false;
    bool _is_body_done = false;
    bool _is_done = false;
    bool _is_open = true;

    std::size_t block_offset = 0;

    SigEntry::parse_buffer sigs_buffer;

    std::vector<uint8_t> body_buffer;

    std::string next_chunk_exts;
    std::optional<http_response::Part> next_chunk_body;
};

using ResourceReader = GenericResourceReader<async_file_handle>;


} // namespace ouinet::cache
