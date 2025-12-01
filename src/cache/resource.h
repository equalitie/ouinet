#pragma once

#include <boost/optional.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include "../util/file_io.h"
#include "../util/hash.h"
#include "../util.h"
#include "../response_reader.h"
#include "http_sign.h"
#include "signed_head.h"

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
    static
    boost::optional<SigEntry>
    parse(Stream& in, parse_buffer& buf, Cancel cancel, asio::yield_context yield)
    {
        sys::error_code ec;
        auto line_len = asio::async_read_until(in, asio::dynamic_buffer(buf), '\n', yield[ec]);
        ec = compute_error_code(ec, cancel);
        if (ec == asio::error::eof) ec = {};
        if (ec) return or_throw(yield, ec, boost::none);

        if (line_len == 0) return boost::none;
        assert(line_len <= buf.size());
        if (buf[line_len - 1] != '\n') {
            CACHE_RESOURCE_ERROR("Truncated signature line");
            return or_throw(yield, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
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
            return or_throw(yield, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
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
    static
    SignedHead read_signed_head(IStream& is, Cancel& cancel, asio::yield_context yield) {
        assert(is.is_open());

        auto on_cancel = cancel.connect([&] { is.close(); });

        // Put in heap to avoid exceeding coroutine stack limit.
        auto buffer = std::make_unique<beast::static_buffer<http_forward_block>>();
        auto parser = std::make_unique<http::response_parser<http::empty_body>>();

        sys::error_code ec;
        http::async_read_header(is, *buffer, *parser, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, SignedHead{});

        if (!parser->is_header_done()) {
            return or_throw<SignedHead>(yield, sys::errc::make_error_code(sys::errc::no_message));
        }

        auto head_o = SignedHead::create_from_trusted_source(parser->release().base());

        if (!head_o) {
            return or_throw<SignedHead>(yield, sys::errc::make_error_code(sys::errc::no_message));
        }

        return std::move(*head_o);
    }

public:
    http_response::Head
    parse_head(Cancel cancel, asio::yield_context yield)
    {
        sys::error_code ec;
        auto head = read_signed_head(headf, cancel, yield[ec]);

        if (ec) {
            if (ec != asio::error::operation_aborted) {
                CACHE_RESOURCE_ERROR("Failed to parse stored response head");
            }
            return or_throw<http_response::Head>(yield, ec);
        }

        uri = std::string(head[http_::response_uri_hdr]);
        if (uri.empty()) {
            CACHE_RESOURCE_ERROR("Missing URI in stored head");
            return or_throw<http_response::Head>(yield, asio::error::bad_descriptor);
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
            size_t ds = 0;
            if (bodyf.is_open()) ds = util::file_io::file_size(bodyf, ec);
            if (ec) return or_throw<http_response::Head>(yield, ec);
            if (range->end > ds) range->end = ds;

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

    void
    seek_to_range_begin(Cancel cancel, asio::yield_context yield)
    {
        assert(_is_head_done);
        if (!range) return;
        if (range->end == 0) return;
        assert(bodyf.is_open());
        assert(block_size);

        sys::error_code ec;

        // Move body file pointer to start of range.
        block_offset = range->begin;
        util::file_io::fseek(bodyf, block_offset, ec);
        if (ec) return or_throw(yield, ec);

        // Consume signatures before the first block.
        for (unsigned b = 0; b < (block_offset / *block_size); ++b) {
            get_sig_entry(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec);
        }
    }

protected:
    boost::optional<SigEntry>
    get_sig_entry(Cancel cancel, asio::yield_context yield)
    {
        assert(_is_head_done);
        if (!sigsf.is_open()) return boost::none;

        return SigEntry::parse(sigsf, sigs_buffer, cancel, yield);
    }

private:
    http_response::ChunkBody
    get_chunk_body(Cancel cancel, asio::yield_context yield)
    {
        assert(_is_head_done);
        http_response::ChunkBody empty_cb{{}, 0};

        if (!bodyf.is_open()) return empty_cb;

        if (body_buffer.size() == 0) {
            assert(block_size);
            body_buffer.resize(*block_size);
        }

        sys::error_code ec;
        auto len = asio::async_read(bodyf, asio::buffer(body_buffer), yield[ec]);
        ec = compute_error_code(ec, cancel);
        if (ec == asio::error::eof) ec = {};
        if (ec) return or_throw(yield, ec, empty_cb);

        assert(len <= body_buffer.size());
        return {std::vector<uint8_t>(body_buffer.cbegin(), body_buffer.cbegin() + len), 0};
    }

    boost::optional<http_response::Part>
    get_chunk_part(Cancel cancel, asio::yield_context yield)
    {
        if (next_chunk_body) {
            // We just sent a chunk header, body comes next.
            auto part = std::move(next_chunk_body);
            next_chunk_body = boost::none;
            return part;
        }

        sys::error_code ec;

        // Get block signature and previous hash,
        // and then its data (which may be empty).
        auto sig_entry = get_sig_entry(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, boost::none);
        // Even if there is no new signature entry,
        // if the signature of the previous block was read
        // it may still be worth sending it in this chunk header
        // (to allow the receiving end to process it).
        // Otherwise it is not worth sending anything.
        if (!sig_entry && next_chunk_exts.empty()) {
            if (!data_size) ec = asio::error::connection_aborted;  // incomplete
            return or_throw(yield, ec, boost::none);
        }
        auto chunk_body = get_chunk_body(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, boost::none);
        // Validate block offset and size.
        if (sig_entry && sig_entry->offset != block_offset) {
            CACHE_RESOURCE_ERROR("Data block offset mismatch: ", sig_entry->offset, " != ", block_offset);
            return or_throw(yield, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
        }
        block_offset += chunk_body.size();

        if (range && block_offset >= range->end) {
            // Hit range end, stop getting more blocks:
            // the next read data block will be empty,
            // thus generating a "last chunk" below.
            sigsf.close();
            bodyf.close();
        }

        if (chunk_body.size() == 0 && next_chunk_exts.empty() && sig_entry)
            // Empty body, generate last chunk header with the signature we just read.
            return http_response::Part(http_response::ChunkHdr(0, sig_entry->chunk_exts()));

        http_response::ChunkHdr ch(chunk_body.size(), next_chunk_exts);
        next_chunk_exts = sig_entry ? sig_entry->chunk_exts() : "";
        if (sig_entry && chunk_body.size() > 0)
            next_chunk_body = std::move(chunk_body);
        return http_response::Part(std::move(ch));
    }

public:
    GenericResourceReader( File headf
                         , File sigsf
                         , File bodyf
                         , boost::optional<Range> range)
        : headf(std::move(headf))
        , sigsf(std::move(sigsf))
        , bodyf(std::move(bodyf))
        , range(range)
    {}

    ~GenericResourceReader() override {};

    boost::optional<ouinet::http_response::Part>
    async_read_part(Cancel cancel, asio::yield_context yield) override
    {
        if (!_is_open || _is_done) return boost::none;

        sys::error_code ec;

        if (!_is_head_done) {
            auto head = parse_head(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec, boost::none);
            _is_head_done = true;
            seek_to_range_begin(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec, boost::none);
            return http_response::Part(std::move(head));
        }

        if (!_is_body_done) {
            auto chunk_part = get_chunk_part(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec, boost::none);
            if (!chunk_part) return boost::none;
            if (auto ch = chunk_part->as_chunk_hdr())
                _is_body_done = (ch->size == 0);  // last chunk
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
        return headf.get_executor();
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
        headf.close();
        sigsf.close();
        bodyf.close();
    }

protected:
    File headf;
    File sigsf;
    File bodyf;

    boost::optional<Range> range;

    std::string uri;  // for warnings
    boost::optional<std::size_t> data_size;
    boost::optional<std::size_t> block_size;

private:
    bool _is_head_done = false;
    bool _is_body_done = false;
    bool _is_done = false;
    bool _is_open = true;

    std::size_t block_offset = 0;

    SigEntry::parse_buffer sigs_buffer;

    std::vector<uint8_t> body_buffer;

    std::string next_chunk_exts;
    boost::optional<http_response::Part> next_chunk_body;
};

using ResourceReader = GenericResourceReader<async_file_handle>;


} // namespace ouinet::cache
