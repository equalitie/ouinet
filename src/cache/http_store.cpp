#include "resource.h"
#include "http_store.h"

#include <array>
#include <ctime>
#include <string>

#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/beast/core/static_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/format.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/optional.hpp>
#include <boost/regex.hpp>
#ifdef _WIN32
#include <tchar.h>
#endif

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
#include "../util/str.h"
#include "../util/variant.h"
#include "signed_head.h"
#include "chain_hasher.h"

#define _LOGPFX "HTTP store: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _WARN(...) LOG_WARN(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)

namespace ouinet { namespace cache {

// An entry modified less than this time ago
// is considered recently updated.
//
// Mainly useful to detect temporary entries that
// are no longer being written to.
static const std::time_t recently_updated_secs = 10 * 60;  // 10 minutes ago

// Lowercase hexadecimal representation of a SHA1 digest,
// split in two.
static const boost::regex parent_name_rx("^[0-9a-f]{2}$");
static const boost::regex dir_name_rx("^[0-9a-f]{38}$");

using Signature = sign::Signature::Bytes;

[[nodiscard]]
static
std::expected<std::size_t, sys::error_code>
recursive_dir_size(const fs::path& path)
{
    // TODO: make asynchronous?
    sys::error_code ec;
    fs::recursive_directory_iterator dit(path, ec);
    if (ec) return std::unexpected(ec);

    // TODO: take directories themselves into account
    // TODO: take block sizes into account
    std::size_t total = 0;
    for (; dit != fs::recursive_directory_iterator(); ++dit) {
        auto p = dit->path();
        auto is_file = fs::is_regular_file(p, ec);
        if (ec) return std::unexpected(ec);
        if (!is_file) continue;
        auto file_size = fs::file_size(p, ec);
        if (ec) return std::unexpected(ec);
        total += file_size;
    }
    return total;
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

class SplittedWriter {
public:
    SplittedWriter(const fs::path& dirp, const AsioExecutor& ex)
        : dirp(dirp), ex(ex) {}

private:
    const fs::path& dirp;
    const AsioExecutor& ex;

    std::string uri;  // for warnings, should use `YieldContext::log` instead
    http_response::Head head;  // for merging in the trailer later on
    boost::optional<async_file_handle> headf, bodyf, sigsf;

    std::size_t block_size;
    std::size_t byte_count = 0;
    unsigned block_count = 0;
    util::SHA512 block_hash;
    ChainHasher chain_hasher;

    [[nodiscard]]
    inline
    std::expected<async_file_handle, sys::error_code>
    create_file(const fs::path& fname)
    {
        auto f = util::file_io::open_or_create(ex, dirp / fname);
        if (!f) return std::unexpected(f.error());
        return std::move(*f);
    }

public:
    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write_part(http_response::Head h, Async yield)
    {
        assert(!headf);

        // Get block size for future alignment checks.
        uri = std::string(h[http_::response_uri_hdr]);
        if (uri.empty()) {
            _ERROR("Missing URI in signed head");
            return std::unexpected(asio::error::invalid_argument);
        }
        auto bsh = h[http_::response_block_signatures_hdr];
        if (bsh.empty()) {
            _ERROR("Missing parameters for data block signatures; uri=", uri);
            return std::unexpected(asio::error::invalid_argument);
        }
        auto bs_params = cache::SignedHead::BlockSigs::parse(bsh);
        if (!bs_params) {
            _ERROR("Malformed parameters for data block signatures; uri=", uri);
            return std::unexpected(asio::error::invalid_argument);
        }
        block_size = bs_params->size;

        // Dump the head without framing headers.
        head = http_injection_merge(std::move(h), {});

        auto hf = create_file(head_fname);
        if (!hf) return std::unexpected(hf.error());
        headf = std::move(*hf);

        return head.async_write(*headf, yield);
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write_part(http_response::ChunkHdr ch, Async yield)
    {
        if (!sigsf) {
            auto sf = create_file(sigs_fname);
            if (!sf) return std::unexpected(sf.error());
            sigsf = std::move(*sf);
        }

        SigEntry e;

        // Only act when a chunk header with a signature is received;
        // upstream verification or the injector should have placed
        // them at the right chunk headers.
        e.signature = std::string(block_sig_from_exts(ch.exts));

        if (e.signature.empty()) return {};

        auto sig = util::base64_decode<sign::Signature::Bytes>(e.signature);

        if (!sig) return {};

        // Check that signature is properly aligned with end of block
        // (except for the last block, which may be shorter).
        e.offset = block_count * block_size;
        block_count++;
        if (ch.size > 0 && byte_count != block_count * block_size) {
            _ERROR("Block signature is not aligned to block boundary; uri=", uri);
            return std::unexpected(asio::error::invalid_argument);
        }

        auto block_digest = block_hash.close();

        e.block_digest = util::base64_encode(block_digest);

        // Encode the chained hash for the previous block.
        if (chain_hasher.prev_chained_digest())
            e.prev_chained_digest = util::base64_encode(*chain_hasher.prev_chained_digest());

        // Prepare hash for next data block: CHASH[i]=SHA2-512(CHASH[i-1] BLOCK[i])
        chain_hasher.calculate_block(ch.size, block_digest, sign::Signature(*sig));

        auto r = util::file_io::write(*sigsf, asio::buffer(e.str()), yield);
        if (!r) return std::unexpected(r.error());
        return {};
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write_part(std::vector<uint8_t> b, Async yield)
    {
        if (!bodyf) {
            auto bf = create_file(body_fname);
            if (!bf) return std::unexpected(bf.error());
            bodyf = std::move(*bf);
        }

        byte_count += b.size();
        block_hash.update(b);
        auto r = util::file_io::write(*bodyf, asio::buffer(b), yield);
        if (!r) return std::unexpected(r.error());
        return {};
    }

    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write_part(http_response::Trailer t, Async yield)
    {
        assert(headf);

        if (t.cbegin() == t.cend()) return {};

        // Extend the head with trailer headers and dump again.
        head = http_injection_merge(std::move(head), t);

        if (auto r = util::file_io::fseek(*headf, 0); !r)
            return std::unexpected(r.error());

        if (auto r = util::file_io::truncate(*headf, 0); !r)
            return std::unexpected(r.error());

        if (auto r = head.async_write(*headf, yield); !r)
            return std::unexpected(r.error());

        return {};
    }
};

std::expected<void, sys::error_code>
http_store(http_response::AbstractReader& reader, const fs::path& dirp, Async yield)
{
    SplittedWriter writer(dirp, yield.get_executor());

    while (true) {
        sys::error_code ec;

        auto r = reader.async_read_part(yield);
        if (!r) return std::unexpected(r.error());

        auto part = std::move(*r);
        if (!part) return {};

        auto ra = util::apply(std::move(*part), [&](auto&& p) {
            return writer.async_write_part(std::move(p), yield);
        });

        if (!ra) return std::unexpected(ra.error());
    }
}

// Since content loaded from the local cache is not verified
// before sending it to the requester,
// we must make extra sure that we are not tricked into reading
// some file outside of the content directory.
static
boost::optional<fs::path>
canonical_from_content_relpath( const fs::path& body_path_p
                              , const fs::path cdirp)
{
    // TODO: proper handling of UTF-8 encoding of body path (including errors)
    fs::path body_rp;
    {  // TODO: supposedly small, so limit size of read data
        std::string body_rp_s;
        boost::nowide::ifstream ifs(body_path_p);
        std::getline(ifs, body_rp_s);
        if (!ifs.fail()) body_rp = body_rp_s;
    }
    if (body_rp.empty()) {
        _ERROR("Failed to read path of static cache content file: ", body_path_p);
        return boost::none;
    }

    // Check correctness of body path.
    if (!body_rp.is_relative()) {
        _ERROR("Path of static cache content file is not relative,"
               " possibly malicious file: ", body_path_p);
        return boost::none;
    }
    for (const auto& c : body_rp)
        if (c.empty() ||
#ifdef _WIN32
            !c.compare(reinterpret_cast<const fs::path::value_type*>(_T("."))) ||
            !c.compare(reinterpret_cast<const fs::path::value_type*>(_T("..")))
#else
            !c.compare(".") ||
            !c.compare("..")
#endif
        ){
            _ERROR("Invalid components in path of static cache content file,"
                   " possibly malicious file: ", body_path_p);
            return boost::none;
        }
    sys::error_code ec;
    auto body_cp = fs::canonical(body_rp, cdirp, ec);
    if (ec) {
        _ERROR( "Failed to get canonical path of static cache content file: ", body_path_p
              , "; ec=", ec);
        return boost::none;
    }
    // Avoid symlinks in actual body path pointing out of content directory.
    auto cdirp_pfx = cdirp / fs::path("/").make_preferred();
    if (body_cp.native().find(cdirp_pfx.native()) != 0) {
        _ERROR("Canonical path of static cache content file outside of content directory,"
               " possibly malicious file: ", body_rp);
        return boost::none;
    }

    return body_cp;
}

static
std::expected<fs::path, sys::error_code>
body_path_external( const fs::path& dirp
                  , const fs::path& cdirp)
{
    fs::path body_path_p = dirp / body_path_fname;
    {
        sys::error_code ec;
        auto body_path_s = fs::status(body_path_p, ec);
        if (!ec && !fs::is_regular_file(body_path_s))
            ec = asio::error::bad_descriptor;
        if (ec) return std::unexpected(ec);
    }

    auto body_cp_o = canonical_from_content_relpath(body_path_p, cdirp);
    if (!body_cp_o) {
        return std::unexpected(asio::error::bad_descriptor);
    }

    return std::move(*body_cp_o);
}

static
std::expected<async_file_handle, sys::error_code>
open_body_external( const AsioExecutor& ex
                  , const fs::path& dirp
                  , const fs::path& cdirp)
{
    auto body_cp = body_path_external(dirp, cdirp);
    if (!body_cp) return std::unexpected(body_cp.error());

    return util::file_io::open_readonly(ex, *body_cp);
}

[[nodiscard]]
static
std::expected<std::size_t, sys::error_code>
body_size_external( const fs::path& dirp
                  , const fs::path& cdirp)
{
    auto body_cp = body_path_external(dirp, cdirp);
    if (!body_cp) return std::unexpected(body_cp.error());

    sys::error_code ec;
    std::size_t size = fs::file_size(*body_cp, ec);
    if (ec) return std::unexpected(ec);
    return size;
}

// `dirp` points to the `/.../data-vX` directory.
// `cdirp` may be set to a directory where an "external" resource body is
// searched for based on the content of `dirp`/`body_path_fname` file.
//
// TODO: It's not clear to me whether `cdirp` is actually being used in
// practice. It also seems very limiting to only have one `body_path_fname`
// reference. So find out whether that code path can be axed.
[[nodiscard]]
static
std::expected<reader_uptr, sys::error_code>
_http_store_reader( const fs::path& dirp, boost::optional<const fs::path&> cdirp
                  , boost::optional<std::size_t> range_first
                  , boost::optional<std::size_t> range_last
                  , Async yield)
{
    sys::error_code ec;
    assert(!cdirp || (fs::canonical(*cdirp, ec) == *cdirp));

    auto ex = yield.get_executor();

    // XXX: Actually the RFC7233 allows for range_last to be undefined
    // https://tools.ietf.org/html/rfc7233#section-2.1
    assert((!range_first && !range_last) || (range_first && range_last));

    auto headf = util::file_io::open_readonly(ex, dirp / head_fname);
    if (!headf) return std::unexpected(headf.error());

    auto head = ResourceReader::read_signed_head(*headf, yield);

    if (!head) {
        CACHE_RESOURCE_ERROR("Failed to parse stored response head");
        return std::unexpected(head.error());
    }

    async_file_handle sigsf(yield.get_executor()), bodyf(yield.get_executor());

    auto sigsf_r = util::file_io::open_readonly(ex, dirp / sigs_fname);

    if (sigsf_r) {
        sigsf = std::move(*sigsf_r);
    }
    else if (sigsf_r.error() != sys::errc::no_such_file_or_directory) {
        return std::unexpected(sigsf_r.error());
    }

    //auto sigsf = util::file_io::open_readonly(ex, dirp / sigs_fname, ec);
    //if (ec && ec != sys::errc::no_such_file_or_directory) return or_throw<reader_uptr>(yield, ec);
    //ec = {};

    auto bodyf_r = util::file_io::open_readonly(ex, dirp / body_fname);

    if (bodyf_r) {
        bodyf = std::move(*bodyf_r);
    }
    else if (bodyf_r.error() == sys::errc::no_such_file_or_directory) {
        if (cdirp) {
            bodyf_r = open_body_external(ex, dirp, *cdirp);
            if (!bodyf_r) return std::unexpected(bodyf_r.error());
            bodyf = std::move(*bodyf_r);
        }
    }
    else {
        return std::unexpected(bodyf_r.error());
    }

    std::optional<Range> range;

    if (range_first) {
        // Check and convert range.
        assert(range_last);
        size_t begin = *range_first;
        size_t end   = *range_last + 1;
        if (begin > end) {
            _WARN("Inverted range boundaries: ", *range_first, " > ", *range_last);
            ec = sys::errc::make_error_code(sys::errc::invalid_seek);
            return std::unexpected(ec);
        }
        if (!bodyf.is_open()) {
            if (begin > 0) {
                _WARN("Positive range requested for response with no stored data");
            }
            begin = 0;
            end = 0;
        } else {
            auto body_size = util::file_io::file_size(bodyf);
            if (!body_size) return std::unexpected(body_size.error());
            if (begin > 0 &&  begin >= *body_size) {
                _WARN( "Requested range 'first' goes beyond stored data: "
                     , util::HttpResponseByteRange{*range_first, *range_last, *body_size});
                ec = sys::errc::make_error_code(sys::errc::invalid_seek);
                return std::unexpected(ec);
            }
            // https://tools.ietf.org/html/rfc7233#section-2.1
            // Quote from the above link: If the last-byte-pos value is absent,
            // or if the value is greater than or equal to the current length
            // of the representation data, the byte range is interpreted as the
            // remainder of the representation (i.e., the server replaces the
            // value of last-byte-pos with a value that is one less than the
            // current length of the selected representation).
            end = std::min(end, *body_size);
        }
        range = Range{begin, end};
    }

    return std::make_unique<ResourceReader>
        (std::move(*head), std::move(sigsf), std::move(bodyf), range);
}

std::expected<reader_uptr, sys::error_code>
http_store_reader( const fs::path& dirp, Async yield)
{
    return _http_store_reader
        (dirp, boost::none, {}, {}, yield);
}

std::expected<reader_uptr, sys::error_code>
http_store_reader( const fs::path& dirp, const fs::path& cdirp, Async yield)
{
    return _http_store_reader
        (dirp, cdirp, {}, {}, yield);
}

std::expected<reader_uptr, sys::error_code>
http_store_range_reader( const fs::path& dirp
                       , std::size_t first, std::size_t last
                       , Async yield)
{
    return _http_store_reader
        (dirp, boost::none, first, last, yield);
}

std::expected<reader_uptr, sys::error_code>
http_store_range_reader( const fs::path& dirp, const fs::path& cdirp
                       , std::size_t first, std::size_t last
                       , Async yield)
{
    return _http_store_reader
        (dirp, cdirp, first, last, yield);
}

std::expected<std::size_t, sys::error_code>
_http_store_body_size( const fs::path& dirp, boost::optional<const fs::path&> cdirp
                     , AsioExecutor ex)
{
    namespace errc = sys::errc;

    sys::error_code ec;

    assert(!cdirp || (fs::canonical(*cdirp, ec) == *cdirp));

    // At least the head file should exist,
    // otherwise opening the body file may fail
    // because the entry does not exist in the cache at all.
    if (!fs::exists(dirp / head_fname, ec)) {
        if (ec) return std::unexpected(ec);
        return std::unexpected(errc::make_error_code(errc::no_such_file_or_directory));
    }

    auto bodysz = fs::file_size(dirp / body_fname, ec);
    if (!ec) return bodysz;
    if (ec != errc::no_such_file_or_directory) return std::unexpected(ec);

    if (!cdirp) return std::unexpected(asio::error::no_data);  // considered incomplete response

    if (auto r = body_size_external(dirp, *cdirp)) {
        return *r;
    }
    else {
        if (r.error() == errc::no_such_file_or_directory) {
            return std::unexpected(asio::error::no_data);
        }
        return std::unexpected(r.error());
    }

    //ec = {};  // retry with content directory
    //bodysz = body_size_external(dirp, *cdirp, ec);
    //if (!ec) return bodysz;
    //if (ec != errc::no_such_file_or_directory) return std::unexpected(ec);

    //ec = asio::error::no_data;
    //return std::unexpected(ec);  // also considered incomplete response
}

std::expected<std::size_t, sys::error_code>
http_store_body_size( const fs::path& dirp, AsioExecutor ex)
{
    return _http_store_body_size(dirp, boost::none, std::move(ex));
}

std::expected<std::size_t, sys::error_code>
http_store_body_size( const fs::path& dirp, const fs::path& cdirp, AsioExecutor ex)
{
    return _http_store_body_size(dirp, cdirp, std::move(ex));
}

fs::path
path_from_resource_id(fs::path dir, const ResourceId& resource_id)
{
    auto hex_digest = resource_id.hex_string();
    boost::string_view hd0(hex_digest); hd0.remove_suffix(hex_digest.size() - 2);
    boost::string_view hd1(hex_digest); hd1.remove_prefix(2);
    return dir.append(hd0.begin(), hd0.end()).append(hd1.begin(), hd1.end());
}

std::expected<HashList, sys::error_code>
http_store_load_hash_list(const fs::path& dir, Async yield)
{
    using Sha = util::SHA512;
    using Digest = Sha::digest_type;

    auto exec = yield.get_executor();

    auto headf = util::file_io::open_readonly(exec, dir / head_fname);
    if (!headf) return std::unexpected(headf.error());

    auto sigsf = util::file_io::open_readonly(exec, dir / sigs_fname);
    if (!sigsf) return std::unexpected(sigsf.error());

    HashList hl;

    if (auto r = ResourceReader::read_signed_head(*headf, yield)) {
        hl.signed_head = std::move(*r);
    }
    else {
        return std::unexpected(r.error());
    }

    std::string sig_buffer;

    while(true) {
        auto opt_sig_entry = SigEntry::parse(*sigsf, sig_buffer, yield);
        if (!opt_sig_entry) return std::unexpected(opt_sig_entry.error());
        if (!*opt_sig_entry) break;
        auto sig_entry = std::move(**opt_sig_entry);

        auto d = util::base64_decode<Digest>(sig_entry.block_digest);
        if (!d) return std::unexpected(asio::error::bad_descriptor);

        auto sig = util::base64_decode<Signature>(sig_entry.signature);
        if (!sig) return std::unexpected(asio::error::bad_descriptor);

        hl.blocks.push_back({*d, { *sig }});
    }

    if (hl.blocks.empty()) {
        return std::unexpected(asio::error::not_found);
    }

    assert(hl.verify()); // Only in debug mode

    return hl;
}

class HttpReadStore : public BaseHttpStore {
public:
    HttpReadStore(fs::path p, AsioExecutor ex)
        : path(std::move(p)), executor(ex)
    {}

    ~HttpReadStore() = default;

    [[nodiscard]]
    std::expected<reader_uptr, sys::error_code>
    reader(const ResourceId& resource_id, Async yield) override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        return http_store_reader(kpath, yield);
    }

    [[nodiscard]]
    std::expected<ReaderAndSize, sys::error_code>
    reader_and_size(const ResourceId& resource_id, Async yield) override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        sys::error_code ec;
        auto rr = http_store_reader(kpath, yield);
        if (!rr) return std::unexpected(rr.error());
        auto bs = http_store_body_size(kpath, executor);
        if (!bs) return std::unexpected(bs.error());
        return ReaderAndSize{std::move(*rr), *bs};
    }

    [[nodiscard]]
    std::expected<reader_uptr, sys::error_code>
    range_reader(const ResourceId& resource_id, size_t first, size_t last, Async yield) override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        return http_store_range_reader(kpath, first, last, yield);
    }

    [[nodiscard]]
    std::expected<std::size_t, sys::error_code>
    body_size(const ResourceId& resource_id) const override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        return http_store_body_size(kpath, executor);
    }

    [[nodiscard]]
    std::expected<std::size_t, sys::error_code>
    size(Async yield) const override
    {
        // Do not use `for_each` since it can alter the store.
        return recursive_dir_size(path);
    }

    [[nodiscard]]
    std::expected<HashList, sys::error_code>
    load_hash_list(const ResourceId& resource_id, Async yield) const override
    {
        auto dir = path_from_resource_id(path, resource_id);
        return http_store_load_hash_list(dir, yield);
    }

protected:
    fs::path path;
    AsioExecutor executor;
};

class StaticHttpStore : public HttpReadStore {
public:
    StaticHttpStore(fs::path p, fs::path cp, sign::PublicKey pk, AsioExecutor ex)
        : HttpReadStore(std::move(p), std::move(ex))
        , content_path(std::move(cp)), verif_pubk(std::move(pk))
    {}

    ~StaticHttpStore() = default;

    [[nodiscard]]
    std::expected<reader_uptr, sys::error_code>
    reader(const ResourceId& resource_id, Async yield) override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        // Always verifying the response not only
        // protects the agent against malicions content in the static cache, it also
        // acts as a good citizen and avoids spreading such content to others.
        sys::error_code ec;
        auto rr = http_store_reader(kpath, content_path, yield);
        if (!rr) return std::unexpected(rr.error());
        return std::make_unique<VerifyingReader>(std::move(*rr), verif_pubk);
    }

    [[nodiscard]]
    std::expected<ReaderAndSize, sys::error_code>
    reader_and_size(const ResourceId& resource_id, Async yield) override
    {
        sys::error_code ec;
        auto kpath = path_from_resource_id(path, resource_id);
        auto r = http_store_reader(kpath, content_path, yield);
        if (!r) return std::unexpected(r.error());
        auto rr = std::make_unique<VerifyingReader>(std::move(*r), verif_pubk);
        auto bs = http_store_body_size(kpath, content_path, executor);
        if (!bs) return std::unexpected(bs.error());
        return ReaderAndSize{std::move(rr), *bs};
    }

    [[nodiscard]]
    std::expected<reader_uptr, sys::error_code>
    range_reader(const ResourceId& resource_id, size_t first, size_t last, Async yield) override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        // TODO: Signature verification should be implemented here too,
        // but verification of partial responses not going through multi-peer download is broken.
        // Fortunately, for agent retrieval of responses in the static cache,
        // only whole responses (returned by `reader`) are used.
        // Also fortunately, other clients retrieving partial content from this client
        // will use the mechanisms of multi-peer download for verification.
        // So this would only byte clients retrieving invalid partial content from here
        // with raw range requests, but this is not currently the case in Ouinet.
        // Also, the client does not currently issue partial reads to the local cache
        // to be served to the agent.
        return http_store_range_reader(kpath, content_path, first, last, yield);
    }

    [[nodiscard]]
    std::expected<std::size_t, sys::error_code>
    body_size(const ResourceId& resource_id) const override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        return http_store_body_size(kpath, content_path, executor);
    }

    [[nodiscard]]
    std::expected<std::size_t, sys::error_code>
    size(Async yield) const override
    {
        auto s0 = HttpReadStore::size(yield);
        if (!s0) return std::unexpected(s0.error());

        auto s1 = recursive_dir_size(content_path);
        if (!s1) return std::unexpected(s1.error());

        return *s0 + *s1;
    }

private:
    fs::path content_path;
    sign::PublicKey verif_pubk;
};

std::unique_ptr<BaseHttpStore>
make_static_http_store( fs::path path, fs::path content_path
                      , sign::PublicKey pk, AsioExecutor ex)
{
    using namespace std;
    return make_unique<StaticHttpStore>(std::move(path), std::move(content_path), std::move(pk), std::move(ex));
}

static
void
try_remove(const fs::path& path)
{
    _DEBUG("Removing cached response: ", path);
    sys::error_code ec;
    fs::remove_all(path, ec);
    if (ec) _WARN( "Failed to remove cached response: "
                 , path, "; ec=", ec);
    // The parent directory may be left empty.
}

static
bool
recently_updated(const fs::path& path)
{
    auto now = std::time(nullptr);

    std::array<fs::path, 4> paths
        { path
        , path / head_fname
        , path / body_fname
        , path / sigs_fname};

    for (const auto& p : paths) {
        sys::error_code ec;
        auto ts = fs::last_write_time(p, ec);
        if (ec) continue;
        if (now - ts <= recently_updated_secs)
            return true;
    }

    return false;
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

class FullHttpStore : public HttpStore {
public:
    FullHttpStore( fs::path p, AsioExecutor ex
                 , std::unique_ptr<BaseHttpStore> rs)
        : path(std::move(p)), executor(std::move(ex))
        , read_store(std::move(rs))
    {}

    ~FullHttpStore() = default;

    std::expected<void, sys::error_code>
    for_each(keep_func, Async) override;

    [[nodiscard]]
    std::expected<void, sys::error_code>
    store( const ResourceId& resource_id, http_response::AbstractReader&, Async) override;

    std::expected<reader_uptr, sys::error_code>
    reader(const ResourceId& resource_id, Async yield) override
    { return read_store->reader(resource_id, yield); }

    [[nodiscard]]
    std::expected<ReaderAndSize, sys::error_code>
    reader_and_size(const ResourceId& resource_id, Async yield) override
    { return read_store->reader_and_size(resource_id, yield); }

    [[nodiscard]]
    std::expected<reader_uptr, sys::error_code>
    range_reader(const ResourceId& resource_id, size_t first, size_t last, Async yield) override
    { return read_store->range_reader(resource_id, first, last, yield); }

    std::expected<std::size_t, sys::error_code>
    body_size(const ResourceId& resource_id) const override
    { return read_store->body_size(resource_id); }

    std::expected<std::size_t, sys::error_code>
    size(Async yield) const override
    { return read_store->size(yield); }

    std::expected<HashList, sys::error_code>
    load_hash_list(const ResourceId& resource_id, Async yield) const override
    { return read_store->load_hash_list(resource_id, yield); }

protected:
    fs::path path;
    AsioExecutor executor;
    std::unique_ptr<BaseHttpStore> read_store;
};

std::expected<void, sys::error_code>
FullHttpStore::for_each(keep_func keep, Async yield)
{
    for (auto& pp : fs::directory_iterator(path)) {  // iterate over `DIGEST[:2]` dirs
        if (!fs::is_directory(pp)) {
            _WARN("Found non-directory: ", pp);
            continue;
        }

        auto pp_name_s = pp.path().filename().native();
        if (!boost::regex_match(pp_name_s.begin(), pp_name_s.end(), parent_name_rx)) {
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
               if (recently_updated(p)) {
                   _DEBUG("Found recent temporary directory: ", p);
               } else {
                   _DEBUG("Found old temporary directory: ", p);
                   try_remove(p);
               }
               continue;
            }

            auto& p_name_s = p_name.native();
            if (!boost::regex_match(p_name_s.begin(), p_name_s.end(), dir_name_rx)) {
                _WARN("Found unknown directory: ", p);
                continue;
            }

            auto resource_id = cache::ResourceId::from_hex(pp_name_s + p_name_s);

            if (!resource_id) {
                _WARN("Item directory is not a valid ResourceId: ", p.path());
                continue;
            }

            sys::error_code ec;

            auto rr = http_store_reader(p, yield);
            if (!rr) {
               _WARN("Failed to open cached response: ", p, "; ec=", rr.error());
               try_remove(p); continue;
            }

            auto keep_entry = keep(*resource_id, std::move(*rr), yield);
            if (!keep_entry) {
                _WARN("Failed to check cached response: ", p, "; ec=", keep_entry.error());
                try_remove(p); continue;
            }

            if (!*keep_entry) try_remove(p);
        }
    }
    return {};
}

std::expected<void, sys::error_code>
FullHttpStore::store(const ResourceId& resource_id, http_response::AbstractReader& reader, Async yield)
{
    sys::error_code ec;

    auto kpath = path_from_resource_id(path, resource_id);

    auto kpath_parent = kpath.parent_path();
    fs::create_directory(kpath_parent, ec);
    if (ec) return std::unexpected(ec);

    // Replacing a directory is not an atomic operation,
    // so try to remove the existing entry before committing.
    auto dir = util::atomic_dir::make(kpath, ec);
    if (ec) return std::unexpected(ec);

    if (auto r = http_store(reader, dir->temp_path(), yield); !r) {
        return std::unexpected(r.error());
    }

    if (fs::exists(kpath)) fs::remove_all(kpath, ec);
    if (ec) return std::unexpected(ec);

    // A new version of the response may still slip in here,
    // but it may be ok since it will probably be recent enough.
    dir->commit(ec);
    if (ec) return std::unexpected(ec);

    if (!ec) {
        _DEBUG("Stored to directory; resource_id=", resource_id, " path=", kpath);
        return {};
    }
    else {
        _ERROR( "Failed to store response; resource_id=", resource_id, " path=", kpath, " ec=", ec);
        return std::unexpected(ec);
    }
}

std::unique_ptr<HttpStore>
make_http_store(fs::path path, AsioExecutor ex)
{
    using namespace std;
    auto read_store = make_unique<HttpReadStore>(path, ex);
    return make_unique<FullHttpStore>(std::move(path), std::move(ex), std::move(read_store));
}

class BackedHttpStore : public FullHttpStore {
public:
    BackedHttpStore( fs::path p, AsioExecutor ex
                   , std::unique_ptr<BaseHttpStore> rs, std::unique_ptr<BaseHttpStore> fs)
        : FullHttpStore(std::move(p), std::move(ex), std::move(rs))
        , fallback_store(std::move(fs))
    {}

    ~BackedHttpStore() = default;

    [[nodiscard]]
    std::expected<reader_uptr, sys::error_code>
    reader(const ResourceId& resource_id, Async yield) override
    {
        auto ret = FullHttpStore::reader(resource_id, yield);
        if (ret) return std::move(*ret);
        _DEBUG("Failed to create reader for resource_id, trying fallback store: ", resource_id);
        return fallback_store->reader(resource_id, yield);
    }

    [[nodiscard]]
    std::expected<ReaderAndSize, sys::error_code>
    reader_and_size(const ResourceId& resource_id, Async yield) override
    {
        auto ret = FullHttpStore::reader_and_size(resource_id, yield);
        if (ret) return std::move(*ret);
        _DEBUG("Failed to create reader for resource_id, trying fallback store: ", resource_id);
        return fallback_store->reader_and_size(resource_id, yield);
    }

    [[nodiscard]]
    std::expected<reader_uptr, sys::error_code>
    range_reader(const ResourceId& resource_id, size_t first, size_t last, Async yield) override
    {
        auto ret = FullHttpStore::range_reader(resource_id, first, last, yield);
        if (ret) return std::move(*ret);
        _DEBUG("Failed to create range reader for resource_id, trying fallback store: ", resource_id);
        return fallback_store->range_reader(resource_id, first, last, yield);
    }

    std::expected<std::size_t, sys::error_code>
    body_size(const ResourceId& resource_id) const override
    {
        auto ret = FullHttpStore::body_size(resource_id);
        if (ret) return std::move(*ret);
        _DEBUG("Failed to get body size for resource_id, trying fallback store: ", resource_id);
        return fallback_store->body_size(resource_id);
    }

    [[nodiscard]]
    std::expected<std::size_t, sys::error_code>
    size(Async yield) const override
    {
        sys::error_code ec;
        auto sz1 = FullHttpStore::size(yield);
        if (!sz1) return std::unexpected(sz1.error());
        auto sz2 = fallback_store->size(yield);
        if (!sz2) return std::unexpected(sz2.error());
        return *sz1 + *sz2;
    }

    [[nodiscard]]
    std::expected<HashList, sys::error_code>
    load_hash_list(const ResourceId& resource_id, Async yield) const override
    {
        auto ret = FullHttpStore::load_hash_list(resource_id, yield);
        if (ret) return std::move(*ret);
        _DEBUG("Failed to load hash list for resource_id, trying fallback store: ", resource_id);
        return fallback_store->load_hash_list(resource_id, yield);
    }

private:
    std::unique_ptr<BaseHttpStore> fallback_store;
};

std::unique_ptr<HttpStore>
make_backed_http_store( fs::path path, std::unique_ptr<BaseHttpStore> fallback_store
                      , AsioExecutor ex)
{
    using namespace std;
    auto read_store = make_unique<HttpReadStore>(path, ex);
    return make_unique<BackedHttpStore>( std::move(path), std::move(ex)
                                       , std::move(read_store), std::move(fallback_store));
}

}} // namespaces
