#include "http_store.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/beast/core/static_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/format.hpp>
#include <boost/optional.hpp>
#include <boost/regex.hpp>

#include "../defer.h"
#include "../http_util.h"
#include "../logger.h"
#include "../or_throw.h"
#include "../parse/number.h"
#include "../util.h"
#include "../util/atomic_dir.h"
#include "../util/atomic_file.h"
#include "../util/bytes.h"
#include "../util/file_io.h"
#include "../util/hash.h"
#include "../util/str.h"
#include "../util/variant.h"
#include "http_sign.h"

#define _LOGPFX "HTTP store: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _WARN(...) LOG_WARN(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)

namespace ouinet { namespace cache {

// Lowercase hexadecimal representation of a SHA1 digest.
static const boost::regex v0_file_name_rx("^[0-9a-f]{40}$");

// Lowercase hexadecimal representation of a SHA1 digest,
// split in two.
static const boost::regex v1_parent_name_rx("^[0-9a-f]{2}$");
static const boost::regex v1_dir_name_rx("^[0-9a-f]{38}$");

// File names for response components.
static const fs::path head_fname = "head";
static const fs::path body_fname = "body";
static const fs::path sigs_fname = "sigs";

static
http_response::Head
without_framing(http_response::Head rsh)
{
    rsh.chunked(false);
    rsh.erase(http::field::content_length);
    rsh.erase(http::field::trailer);
    return rsh;
}

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

static
std::size_t
parse_data_block_offset(const std::string& s)  // `^[0-9a-f]*$`
{
    std::size_t offset = 0;
    for (auto& c : s) {
        assert(('0' <= c && c <= '9') || ('a' <= c && c <= 'f'));
        offset <<= 4;
        offset += ('0' <= c && c <= '9') ? c - '0' : c - 'a';
    }
    return offset;
}

// A signatures file entry with `OFFSET[i] SIGNATURE[i] HASH[i-1]`.
struct SigEntry {
    std::size_t offset;
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
        auto offset = parse_data_block_offset(m[1].str());
        SigEntry entry{offset, m[2].str(), m[3].str()};
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

    std::size_t block_size;
    std::size_t byte_count = 0;
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
http_store_v1( http_response::AbstractReader& reader, const fs::path& dirp
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

reader_uptr
http_store_reader_v0(const fs::path& path, asio::executor ex, sys::error_code& ec)
{
    auto file = util::file_io::open_readonly(ex, path, ec);
    if (ec) return nullptr;
    return std::make_unique<http_response::Reader>(std::move(file));
}

class HttpStore1Reader : public http_response::AbstractReader {
private:
    static const std::size_t http_forward_block = 16384;

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
        auto data_size_opt = parse::number<std::size_t>(data_size_hdr);
        if (!data_size_opt)
            _WARN("Loading incomplete stored response; uri=", uri);
        else
            data_size = *data_size_opt;

        // Create a partial content response if a range was specified.
        if (range_begin) {
            assert(range_end);

            auto orig_status = head.result_int();
            head.reason("");
            head.result(http::status::partial_content);
            head.set(http_::response_original_http_status, orig_status);

            // Align ranges to data blocks.
            assert(block_size);
            auto bs = *block_size;
            range_begin = bs * (*range_begin / bs);  // align down
            range_end = bs * (*range_end / bs + 1);  // align up
            // Clip range end to actual file size.
            auto ds = util::file_io::file_size(bodyf, ec);
            if (ec) return or_throw<http_response::Head>(yield, ec);
            if (*range_end > ds) range_end = ds;

            // Report resulting range.
            head.set( http::field::content_range
                    , util::HttpByteRange{*range_begin, *range_end - 1, data_size});
        }

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

    void
    seek_to_range_begin(Cancel cancel, asio::yield_context yield)
    {
        assert(_is_head_done);
        if (!range_begin) return;
        assert(bodyf.is_open());
        assert(block_size);

        sys::error_code ec;

        // Move body file pointer to start of range.
        block_offset = *range_begin;
        util::file_io::fseek(bodyf, block_offset, ec);
        if (ec) return or_throw(yield, ec);

        // Consume signatures before the first block.
        for (unsigned b = 0; b < (block_offset / *block_size); ++b) {
            get_sig_entry(cancel, yield[ec]);
            return_or_throw_on_error(yield, cancel, ec);
        }
    }

    boost::optional<SigEntry>
    get_sig_entry(Cancel cancel, asio::yield_context yield)
    {
        assert(_is_head_done);
        if (!sigsf.is_open()) return boost::none;

        if (sigs_buffer.size() == 0)
            sigs_buffer = SigEntry::create_parse_buffer();

        return SigEntry::parse(sigsf, sigs_buffer, cancel, yield);
    }

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
        if (cancel) ec == asio::error::operation_aborted;
        if (ec == asio::error::eof) ec = {};
        return_or_throw_on_error(yield, cancel, ec, empty_cb);

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
        if (!sig_entry && next_chunk_exts.empty())
            return boost::none;
        auto chunk_body = get_chunk_body(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, boost::none);
        // Validate block offset and size.
        if (sig_entry && sig_entry->offset != block_offset) {
            _ERROR("Data block offset mismatch: ", sig_entry->offset, " != ", block_offset);
            return or_throw(yield, sys::errc::make_error_code(sys::errc::bad_message), boost::none);
        }
        block_offset += chunk_body.size();

        if (range_end && block_offset >= *range_end) {
            // Hit range end, stop getting more blocks:
            // the next read data block will be empty,
            // thus generating a "last chunk" below.
            sigsf.close();
            bodyf.close();
        }

        http_response::ChunkHdr ch(chunk_body.size(), next_chunk_exts);
        next_chunk_exts = sig_entry ? sig_entry->chunk_exts() : "";
        if (sig_entry && chunk_body.size() > 0)
            next_chunk_body = std::move(chunk_body);
        return http_response::Part(std::move(ch));
    }

public:
    HttpStore1Reader( asio::posix::stream_descriptor headf
                    , asio::posix::stream_descriptor sigsf
                    , asio::posix::stream_descriptor bodyf
                    , boost::optional<std::size_t> range_begin
                    , boost::optional<std::size_t> range_end)
        : headf(std::move(headf))
        , sigsf(std::move(sigsf))
        , bodyf(std::move(bodyf))
        , range_begin(std::move(range_begin))
        , range_end(std::move(range_end))
    {}

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
        sigsf.close();
        bodyf.close();
    }

private:
    asio::posix::stream_descriptor headf;
    asio::posix::stream_descriptor sigsf;
    asio::posix::stream_descriptor bodyf;

    boost::optional<std::size_t> range_begin, range_end;

    bool _is_head_done = false;
    bool _is_body_done = false;
    bool _is_done = false;
    bool _is_open = true;

    std::string uri;  // for warnings
    std::size_t block_offset = 0;
    boost::optional<std::size_t> data_size;
    boost::optional<std::size_t> block_size;

    SigEntry::parse_buffer sigs_buffer;

    std::vector<uint8_t> body_buffer;

    std::string next_chunk_exts;
    boost::optional<http_response::Part> next_chunk_body;
};

static
reader_uptr
_http_store_reader_v1( const fs::path& dirp, asio::executor ex
                     , boost::optional<std::size_t> range_first
                     , boost::optional<std::size_t> range_last
                     , sys::error_code& ec)
{
    auto headf = util::file_io::open_readonly(ex, dirp / head_fname, ec);
    if (ec) return nullptr;

    auto sigsf = util::file_io::open_readonly(ex, dirp / sigs_fname, ec);
    if (ec && ec != sys::errc::no_such_file_or_directory) return nullptr;

    auto bodyf = util::file_io::open_readonly(ex, dirp / body_fname, ec);
    if (ec && ec != sys::errc::no_such_file_or_directory) return nullptr;

    boost::optional<std::size_t> range_begin, range_end;
    if (range_first) {
        // Check and convert range.
        assert(range_last);
        if (*range_first > *range_last) {
            _WARN("Inverted range boundaries: ", *range_first, " > ", *range_last);
            ec = sys::errc::make_error_code(sys::errc::invalid_seek);
            return nullptr;
        }
        if (!bodyf.is_open()) {
            _WARN("Range requested for response with no stored data");
            ec = sys::errc::make_error_code(sys::errc::invalid_seek);
            return nullptr;
        }
        auto body_size = util::file_io::file_size(bodyf, ec);
        if (ec) return nullptr;
        if ( *range_first >= body_size
           || *range_last >= body_size) {
            _WARN( "Requested range goes beyond stored data: "
                 , util::HttpByteRange{*range_first, *range_last, body_size});
            ec = sys::errc::make_error_code(sys::errc::invalid_seek);
            return nullptr;
        }
        range_begin = *range_first;
        range_end = *range_last + 1;
    }

    return std::make_unique<HttpStore1Reader>
        ( std::move(headf), std::move(sigsf), std::move(bodyf)
        , std::move(range_begin), std::move(range_end));
}

reader_uptr
http_store_reader_v1( const fs::path& dirp, asio::executor ex
                    , sys::error_code& ec)
{
    return _http_store_reader_v1(dirp, std::move(ex), {}, {}, ec);
}

reader_uptr
http_store_range_reader_v1( const fs::path& dirp, asio::executor ex
                          , std::size_t first, std::size_t last
                          , sys::error_code& ec)
{
    return _http_store_reader_v1(dirp, std::move(ex), first, last, ec);
}

class HttpStore1HeadReader : public http_response::AbstractReader {
public:
    HttpStore1HeadReader( reader_uptr reader
                        , asio::posix::stream_descriptor sigsf)
        : reader(std::move(reader))
        , sigsf(std::move(sigsf))
    {}

    ~HttpStore1HeadReader() override {};

    boost::optional<ouinet::http_response::Part>
    async_read_part(Cancel cancel, asio::yield_context yield) override
    {
        if (!_is_open || _is_done) return boost::none;

        sys::error_code ec;
        auto part_o = reader->async_read_part(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, boost::none);
        assert(part_o);
        auto head_p = part_o->as_head();
        assert(head_p);
        // According to RFC7231#4.3.2, payload header fields MAY be omitted.
        auto head = without_framing(std::move(*head_p));
        // TODO: get last useable byte, add header
        _is_done = true;
        close();
        return http_response::Part(std::move(head));
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
        reader->close();
    }

private:
    reader_uptr reader;
    asio::posix::stream_descriptor sigsf;

    bool _is_done = false;
    bool _is_open = true;
};

reader_uptr
http_store_head_reader_v1( const fs::path& dirp, asio::executor ex
                         , sys::error_code& ec)
{
    auto sigsf = util::file_io::open_readonly(ex, dirp / sigs_fname, ec);
    if (ec && ec != sys::errc::no_such_file_or_directory) return nullptr;

    auto reader = http_store_reader_v1(dirp, std::move(ex), ec);
    if (ec) return nullptr;

    return std::make_unique<HttpStore1HeadReader>
        (std::move(reader), std::move(sigsf));
}

// begin HttpStoreV0

HttpStoreV0::~HttpStoreV0()
{
}

static
fs::path
v0_path_from_key(const fs::path& dir, const std::string& key)
{
    auto key_digest = util::sha1_digest(key);
    return dir / util::bytes::to_hex(key_digest);
}

static
void
v0_try_remove(const fs::path& path)
{
    _DEBUG("Removing cached response: ", path);
    sys::error_code ec;
    fs::remove(path, ec);
    if (ec) _WARN( "Failed to remove cached response: "
                 , path, " ec:", ec.message());
}

// For instance, "tmp.1234-abcd" matches "tmp.%%%%-%%%%".
static
bool
name_matches_model(const fs::path& name, const fs::path& model)
{
    if (name.size() != model.size())
        return false;

    auto& name_s = name.native();
    auto& model_s = model.native();
    for (size_t i = 0; i < model.size(); ++i)
        // This is simplified, actually "%" becomes lowercase hex.
        if (model_s[i] != '%' && (model_s[i] != name_s[i]))
            return false;

    return true;
}

void
HttpStoreV0::for_each(keep_func keep, asio::yield_context yield)
{
    for (auto& p : fs::directory_iterator(path)) {
        if (!fs::is_regular_file(p)) {
            _WARN("Found non-regular file: ", p);
            continue;
        }

        auto p_name = p.path().filename();
        if (name_matches_model(p_name, util::default_temp_model)) {
            _DEBUG("Found temporary file: ", p);
            v0_try_remove(p); continue;
        }

        auto& p_name_s = p_name.native();
        if (!boost::regex_match(p_name_s.begin(), p_name_s.end(), v0_file_name_rx)) {
            _WARN("Found unknown file: ", p);
            continue;
        }

        sys::error_code ec;

        auto rr = http_store_reader_v0(p, executor, ec);
        if (ec == asio::error::operation_aborted) return;
        if (ec) {
            _WARN("Failed to open cached response: ", p, " ec:", ec.message());
            v0_try_remove(p); continue;
        }
        assert(rr);

        auto keep_entry = keep(std::move(rr), yield[ec]);
        if (ec == asio::error::operation_aborted) return;
        if (ec) {
            _WARN("Failed to check cached response: ", p, " ec:", ec.message());
            v0_try_remove(p); continue;
        }

        if (!keep_entry)
            v0_try_remove(p);
    }
}

void
HttpStoreV0::store( const std::string& key, http_response::AbstractReader& r
                  , Cancel cancel, asio::yield_context yield)
{
    sys::error_code ec;

    auto kpath = v0_path_from_key(path, key);
    auto file = util::atomic_file::make(executor, kpath, ec);
    if (!ec) http_store_v0(r, *file, cancel, yield[ec]);
    if (!ec) file->commit(ec);
    if (!ec) _DEBUG("Stored to file; key=", key, " path=", kpath);
    else _ERROR( "Failed to store response; key=", key, " path=", kpath
               , " ec:", ec.message());
    return or_throw(yield, ec);
}

reader_uptr
HttpStoreV0::reader( const std::string& key
                   , sys::error_code& ec)
{
    auto kpath = v0_path_from_key(path, key);
    return http_store_reader_v0(kpath, executor, ec);
}

// end HttpStoreV0

// begin HttpStoreV0

HttpStoreV1::~HttpStoreV1()
{
}

static
fs::path
v1_path_from_key(fs::path dir, const std::string& key)
{
    auto key_digest = util::sha1_digest(key);
    auto hex_digest = util::bytes::to_hex(key_digest);
    boost::string_view hd0(hex_digest); hd0.remove_suffix(hex_digest.size() - 2);
    boost::string_view hd1(hex_digest); hd1.remove_prefix(2);
    return dir.append(hd0.begin(), hd0.end()).append(hd1.begin(), hd1.end());
}

static
void
v1_try_remove(const fs::path& path)
{
    _DEBUG("Removing cached response: ", path);
    sys::error_code ec;
    fs::remove_all(path, ec);
    if (ec) _WARN( "Failed to remove cached response: "
                 , path, " ec:", ec.message());
    // The parent directory may be left empty.
}

void
HttpStoreV1::for_each(keep_func keep, asio::yield_context yield)
{
    for (auto& pp : fs::directory_iterator(path)) {  // iterate over `DIGEST[:2]` dirs
        if (!fs::is_directory(pp)) {
            _WARN("Found non-directory: ", pp);
            continue;
        }

        auto pp_name_s = pp.path().filename().native();
        if (!boost::regex_match(pp_name_s.begin(), pp_name_s.end(), v1_parent_name_rx)) {
            _WARN("Found unknown directory: ", pp);
            continue;
        }

        for (auto& p : fs::directory_iterator(pp)) {  // iterate over `DIGEST[2:]` dirs
            if (!fs::is_directory(p)) {
                _WARN("Found non-directory: ", p);
                continue;
            }

            auto p_name = p.path().filename();
            if (name_matches_model(p_name, util::default_temp_model)) {
                _DEBUG("Found temporary directory: ", p);
                v1_try_remove(p); continue;
            }

            auto& p_name_s = p_name.native();
            if (!boost::regex_match(p_name_s.begin(), p_name_s.end(), v1_dir_name_rx)) {
                _WARN("Found unknown directory: ", p);
                continue;
            }

            sys::error_code ec;

            auto rr = http_store_reader_v1(p, executor, ec);
            if (ec == asio::error::operation_aborted) return;
            if (ec) {
               _WARN("Failed to open cached response: ", p, " ec:", ec.message());
               v1_try_remove(p); continue;
            }
            assert(rr);

            auto keep_entry = keep(std::move(rr), yield[ec]);
            if (ec == asio::error::operation_aborted) return;
            if (ec) {
                _WARN("Failed to check cached response: ", p, " ec:", ec.message());
                v1_try_remove(p); continue;
            }

            if (!keep_entry)
                v1_try_remove(p);
        }
    }
}

void
HttpStoreV1::store( const std::string& key, http_response::AbstractReader& r
                  , Cancel cancel, asio::yield_context yield)
{
    sys::error_code ec;

    auto kpath = v1_path_from_key(path, key);

    auto kpath_parent = kpath.parent_path();
    fs::create_directory(kpath_parent, ec);
    if (ec) return or_throw(yield, ec);

    // Replacing a directory is not an atomic operation,
    // so try to remove the existing entry before committing.
    auto dir = util::atomic_dir::make(kpath, ec);
    if (!ec) http_store_v1(r, dir->temp_path(), executor, cancel, yield[ec]);
    if (!ec && fs::exists(kpath)) fs::remove_all(kpath, ec);
    // A new version of the response may still slip in here,
    // but it may be ok since it will probably be recent enough.
    if (!ec) dir->commit(ec);
    if (!ec) _DEBUG("Stored to directory; key=", key, " path=", kpath);
    else _ERROR( "Failed to store response; key=", key, " path=", kpath
               , " ec:", ec.message());
    return or_throw(yield, ec);
}

reader_uptr
HttpStoreV1::reader( const std::string& key
                   , sys::error_code& ec)
{
    auto kpath = v1_path_from_key(path, key);
    return http_store_reader_v1(kpath, executor, ec);
}

// end HttpStoreV1

}} // namespaces
