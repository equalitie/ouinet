#include "http_store.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/beast/core/static_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/format.hpp>
#include <boost/optional.hpp>
#include <boost/regex.hpp>

#include "../defer.h"
#include "../logger.h"
#include "../or_throw.h"
#include "../parse/number.h"
#include "../util.h"
#include "../util/file_io.h"
#include "../util/variant.h"
#include "http_sign.h"

#define _LOGPFX "HTTP store: "
#define _WARN(...) LOG_WARN(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)

namespace ouinet { namespace cache {

// File names for response components.
static const fs::path head_fname = "head";
static const fs::path body_fname = "body";
static const fs::path sigs_fname = "sigs";

// Block signature and hash handling.
static
boost::string_view
block_sig_from_exts(boost::string_view xs)
{
    // Simplified chunk extension parsing
    // since this should have already been validated upstream.
    static const std::string sigpfx = ";" + http_::response_block_signature_ext + "=\"";
    auto sigext = xs.find(sigpfx);
    if (sigext == std::string::npos) return {};  // no such extension
    auto sigstart = sigext + sigpfx.size();
    assert(sigstart < xs.size());
    auto sigend = xs.find('"', sigstart);
    assert(sigend != std::string::npos);
    return xs.substr(sigstart, sigend - sigstart);
}

// A signatures file entry with `OFFSET[i] SIGNATURE[i] HASH[i-1]`.
struct SigEntry {
    size_t offset;
    std::string signature;
    std::string prev_digest;

    using parse_buffer = std::string;

    std::string str() const
    {
        static const auto line_format = "%x %s %s\n";
        return (boost::format(line_format) % offset % signature % prev_digest).str();
    }

    std::string chunk_exts() const
    {
        std::stringstream exts;

        static const auto fmt_sx = ";" + http_::response_block_signature_ext + "=\"%s\"";
        if (!signature.empty())
            exts << (boost::format(fmt_sx) % signature);

        static const auto fmt_hx = ";" + http_::response_block_chain_hash_ext + "=\"%s\"";
        if (!prev_digest.empty())
            exts << (boost::format(fmt_hx) % prev_digest);

        return exts.str();
    }

    static
    parse_buffer create_parse_buffer()
    {
        // We may use a flat buffer or something like that,
        // but this will suffice for the moment.
        return {};
    }

    template<class Stream>
    static
    boost::optional<SigEntry>
    parse(Stream& in, parse_buffer& buf, Cancel cancel, asio::yield_context yield)
    {
        sys::error_code ec;
        auto line_len = asio::async_read_until(in, asio::dynamic_buffer(buf), '\n', yield[ec]);
        if (cancel) ec = asio::error::operation_aborted;
        if (ec == asio::error::eof) ec = {};
        return_or_throw_on_error(yield, cancel, ec, boost::none);

        if (line_len == 0) return boost::none;
        assert(line_len <= buf.size());
        if (buf[line_len - 1] != '\n') {
            _ERROR("Truncated signature line");
            return or_throw(yield, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
        }
        boost::string_view line(buf);
        line.remove_suffix(buf.size() - line_len + 1);  // leave newline out

        static const boost::regex line_regex(
            "([0-9a-f]+)"  // LOWER_HEX(OFFSET[i])
            " ([A-Za-z0-9+/]+=*)"  // BASE64(SIG[i])
            " ([A-Za-z0-9+/]+=*)?"  // BASE64(HASH([i-1]))
        );
        boost::cmatch m;
        if (!boost::regex_match(line.begin(), line.end(), m, line_regex)) {
            _ERROR("Malformed signature line");
            return or_throw(yield, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
        }
        auto offset_s = m[1].str();
        boost::string_view offset_sv(offset_s);
        auto offset = parse::number<size_t>(offset_sv); assert(offset);
        SigEntry entry{*offset, m[2].str(), m[3].str()};
        buf.erase(0, line_len);  // consume used input
        return entry;
    }
};

class SplittedWriter {
public:
    SplittedWriter(const fs::path& dirp, const asio::executor& ex)
        : dirp(dirp), ex(ex) {}

private:
    const fs::path& dirp;
    const asio::executor& ex;

    std::string uri;  // for warnings, should use `Yield::log` instead
    http_response::Head head;  // for merging in the trailer later on
    boost::optional<asio::posix::stream_descriptor> headf, bodyf, sigsf;

    size_t block_size;
    size_t byte_count = 0;
    unsigned block_count = 0;
    util::SHA512 block_hash;
    boost::optional<util::SHA512::digest_type> prev_block_digest;

    inline
    asio::posix::stream_descriptor
    create_file(const fs::path& fname, Cancel cancel, sys::error_code& ec)
    {
        auto f = util::file_io::open_or_create(ex, dirp / fname, ec);
        if (cancel) ec = asio::error::operation_aborted;
        return f;
    }

public:
    void
    async_write_part(http_response::Head h, Cancel cancel, asio::yield_context yield)
    {
        assert(!headf);

        // Get block size for future alignment checks.
        uri = h[http_::response_uri_hdr].to_string();
        if (uri.empty()) {
            _ERROR("Missing URI in signed head");
            return or_throw(yield, asio::error::invalid_argument);
        }
        auto bsh = h[http_::response_block_signatures_hdr];
        if (bsh.empty()) {
            _ERROR("Missing parameters for data block signatures; uri=", uri);
            return or_throw(yield, asio::error::invalid_argument);
        }
        auto bs_params = cache::HttpBlockSigs::parse(bsh);
        if (!bs_params) {
            _ERROR("Malformed parameters for data block signatures; uri=", uri);
            return or_throw(yield, asio::error::invalid_argument);
        }
        block_size = bs_params->size;

        // Dump the head without framing headers.
        head = http_injection_merge(std::move(h), {});

        sys::error_code ec;
        auto hf = create_file(head_fname, cancel, ec);
        return_or_throw_on_error(yield, cancel, ec);
        headf = std::move(hf);
        head.async_write(*headf, cancel, yield);
    }

    void
    async_write_part(http_response::ChunkHdr ch, Cancel cancel, asio::yield_context yield)
    {
        if (!sigsf) {
            sys::error_code ec;
            auto sf = create_file(sigs_fname, cancel, ec);
            return_or_throw_on_error(yield, cancel, ec);
            sigsf = std::move(sf);
        }

        SigEntry e;

        // Only act when a chunk header with a signature is received;
        // upstream verification or the injector should have placed
        // them at the right chunk headers.
        e.signature = block_sig_from_exts(ch.exts).to_string();
        if (e.signature.empty()) return;

        // Check that signature is properly aligned with end of block
        // (except for the last block, which may be shorter).
        e.offset = block_count * block_size;
        block_count++;
        if (ch.size > 0 && byte_count != block_count * block_size) {
            _ERROR("Block signature is not aligned to block boundary; uri=", uri);
            return or_throw(yield, asio::error::invalid_argument);
        }

        // Encode the chained hash for the previous block.
        if (prev_block_digest)
            e.prev_digest = util::base64_encode(*prev_block_digest);

        // Prepare hash for next data block: HASH[i]=SHA2-512(HASH[i-1] BLOCK[i])
        prev_block_digest = block_hash.close();
        block_hash = {}; block_hash.update(*prev_block_digest);

        util::file_io::write(*sigsf, asio::buffer(e.str()), cancel, yield);
    }

    void
    async_write_part(std::vector<uint8_t> b, Cancel cancel, asio::yield_context yield)
    {
        if (!bodyf) {
            sys::error_code ec;
            auto bf = create_file(body_fname, cancel, ec);
            return_or_throw_on_error(yield, cancel, ec);
            bodyf = std::move(bf);
        }

        byte_count += b.size();
        block_hash.update(b);
        util::file_io::write(*bodyf, asio::buffer(b), cancel, yield);
    }

    void
    async_write_part(http_response::Trailer t, Cancel cancel, asio::yield_context yield)
    {
        assert(headf);

        if (t.cbegin() == t.cend()) return;

        // Extend the head with trailer headers and dump again.
        head = http_injection_merge(std::move(head), t);

        sys::error_code ec;
        util::file_io::fseek(*headf, 0, ec);
        if (!ec) util::file_io::truncate(*headf, 0, ec);
        if (!ec) head.async_write(*headf, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
    }
};

void
http_store( http_response::AbstractReader& reader, const fs::path& dirp
          , const asio::executor& ex, Cancel cancel, asio::yield_context yield)
{
    SplittedWriter writer(dirp, ex);

    while (true) {
        sys::error_code ec;

        auto part = reader.async_read_part(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
        if (!part) break;

        util::apply(std::move(*part), [&](auto&& p) {
            writer.async_write_part(std::move(p), cancel, yield[ec]);
        });
        return_or_throw_on_error(yield, cancel, ec);
    }
}

class HttpStore1Reader : public http_response::AbstractReader {
private:
    static const size_t http_forward_block = 16384;

    http_response::Head
    parse_head(Cancel cancel, asio::yield_context yield)
    {
        assert(headf.is_open());
        auto close_headf = defer([&headf = headf] { headf.close(); });  // no longer needed

        // Put in heap to avoid exceeding coroutine stack limit.
        auto buffer = std::make_unique<beast::static_buffer<http_forward_block>>();
        auto parser = std::make_unique<http::response_parser<http::empty_body>>();

        sys::error_code ec;
        http::async_read_header(headf, *buffer, *parser, yield[ec]);
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<http_response::Head>(yield, ec);

        if (!parser->is_header_done()) {
            _ERROR("Failed to parse stored response head");
            return or_throw<http_response::Head>(yield, sys::errc::make_error_code(sys::errc::no_message));
        }

        auto head = parser->release().base();
        uri = head[http_::response_uri_hdr].to_string();
        if (uri.empty()) {
            _ERROR("Missing URI in stored head");
            return or_throw<http_response::Head>(yield, sys::errc::make_error_code(sys::errc::no_message));
        }
        auto bsh = head[http_::response_block_signatures_hdr];
        if (bsh.empty()) {
            _ERROR("Missing stored parameters for data block signatures; uri=", uri);
            return or_throw<http_response::Head>(yield, sys::errc::make_error_code(sys::errc::no_message));
        }
        auto bs_params = cache::HttpBlockSigs::parse(bsh);
        if (!bs_params) {
            _ERROR("Malformed stored parameters for data block signatures; uri=", uri);
            return or_throw<http_response::Head>(yield, sys::errc::make_error_code(sys::errc::no_message));
        }
        block_size = bs_params->size;
        auto data_size_hdr = head[http_::response_data_size_hdr];
        auto data_size_opt = parse::number<size_t>(data_size_hdr);
        if (!data_size_opt)
            _WARN("Loading incomplete stored response; uri=", uri);
        else
            data_size = *data_size_opt;

        // The stored head should not have framing headers,
        // check and enable chunked transfer encoding.
        if (!( head[http::field::content_length].empty()
             && head[http::field::transfer_encoding].empty()
             && head[http::field::trailer].empty())) {
            _WARN("Found framing headers in stored head, cleaning; uri=", uri);
            head = http_injection_merge(std::move(head), {});
        }
        head.set(http::field::transfer_encoding, "chunked");
        return head;
    }

    boost::optional<SigEntry>
    get_sig_entry(Cancel cancel, asio::yield_context yield)
    {
        assert(_is_head_done);
        if (!sigsf) {
            sys::error_code ec;
            sigsf = util::file_io::open_readonly(ex, dirp / sigs_fname, ec);
            if (ec == sys::errc::no_such_file_or_directory)
                return boost::none;
            return_or_throw_on_error(yield, cancel, ec, boost::none);

            sigs_buffer = SigEntry::create_parse_buffer();
        }
        return SigEntry::parse(*sigsf, sigs_buffer, cancel, yield);
    }

    http_response::ChunkBody
    get_chunk_body(Cancel cancel, asio::yield_context yield)
    {
        return {{}, 0};  // TODO: implement
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
        // and only then its data (which may be empty).
        auto sig_entry = get_sig_entry(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, boost::none);
        if (!sig_entry) return boost::none;
        auto chunk_body = get_chunk_body(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, boost::none);
        // TODO: validate offset and size

        http_response::ChunkHdr ch(chunk_body.size(), next_chunk_exts);
        next_chunk_exts = sig_entry->chunk_exts();
        if (chunk_body.size() > 0)
            next_chunk_body = std::move(chunk_body);
        return http_response::Part(std::move(ch));
    }

public:
    HttpStore1Reader( fs::path dirp
                    , asio::posix::stream_descriptor headf
                    , asio::executor ex)
        : dirp(std::move(dirp)), headf(std::move(headf)), ex(std::move(ex)) {}

    ~HttpStore1Reader() override {};

    boost::optional<ouinet::http_response::Part>
    async_read_part(Cancel cancel, asio::yield_context yield) override
    {
        if (!_is_open || _is_done) return boost::none;

        sys::error_code ec;

        if (!_is_head_done) {
            auto head = parse_head(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec, boost::none);
            _is_head_done = true;
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

    bool
    is_done() const override
    {
        return _is_done;
    }

    bool
    is_open() const override
    {
        return _is_open;
    }

    void
    close() override
    {
        _is_open = false;
        headf.close();
        if (sigsf) sigsf->close();
    }

private:
    const fs::path dirp;
    asio::posix::stream_descriptor headf;
    asio::executor ex;

    bool _is_head_done = false;
    bool _is_body_done = false;
    bool _is_done = false;
    bool _is_open = true;

    std::string uri;  // for warnings
    boost::optional<size_t> data_size;
    boost::optional<size_t> block_size;

    boost::optional<asio::posix::stream_descriptor> sigsf;
    SigEntry::parse_buffer sigs_buffer;

    std::string next_chunk_exts;
    boost::optional<http_response::Part> next_chunk_body;
};

std::unique_ptr<http_response::AbstractReader>
http_store_reader_v1(fs::path dirp, asio::executor ex, sys::error_code& ec)
{
    auto headf = util::file_io::open_readonly(ex, dirp / head_fname, ec);
    if (ec) return nullptr;

    return std::make_unique<HttpStore1Reader>(std::move(dirp), std::move(headf), std::move(ex));
}

}} // namespaces
