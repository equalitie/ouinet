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

// File names for response components.
static const fs::path head_fname = "head";
static const fs::path body_fname = "body";
static const fs::path body_path_fname = "body-path";
static const fs::path sigs_fname = "sigs";

using Signature = util::Ed25519PublicKey::sig_array_t;

static
std::size_t
recursive_dir_size(const fs::path& path, sys::error_code& ec)
{
    // TODO: make asynchronous?
    fs::recursive_directory_iterator dit(path, ec);
    if (ec) return 0;

    // TODO: take directories themselves into account
    // TODO: take block sizes into account
    std::size_t total = 0;
    for (; dit != fs::recursive_directory_iterator(); ++dit) {
        auto p = dit->path();
        auto is_file = fs::is_regular_file(p, ec);
        if (ec) return 0;
        if (!is_file) continue;
        auto file_size = fs::file_size(p, ec);
        if (ec) return 0;
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

    inline
    async_file_handle
    create_file(const fs::path& fname, Cancel cancel, sys::error_code& ec)
    {
        auto f = util::file_io::open_or_create(ex, dirp / fname, ec);
        ec = compute_error_code(ec, cancel);
        return f;
    }

public:
    void
    async_write_part(http_response::Head h, Cancel cancel, asio::yield_context yield)
    {
        assert(!headf);

        // Get block size for future alignment checks.
        uri = std::string(h[http_::response_uri_hdr]);
        if (uri.empty()) {
            _ERROR("Missing URI in signed head");
            return or_throw(yield, asio::error::invalid_argument);
        }
        auto bsh = h[http_::response_block_signatures_hdr];
        if (bsh.empty()) {
            _ERROR("Missing parameters for data block signatures; uri=", uri);
            return or_throw(yield, asio::error::invalid_argument);
        }
        auto bs_params = cache::SignedHead::BlockSigs::parse(bsh);
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
        e.signature = std::string(block_sig_from_exts(ch.exts));

        if (e.signature.empty()) return;

        auto sig = util::base64_decode<Signature>(e.signature);

        if (!sig) return;

        // Check that signature is properly aligned with end of block
        // (except for the last block, which may be shorter).
        e.offset = block_count * block_size;
        block_count++;
        if (ch.size > 0 && byte_count != block_count * block_size) {
            _ERROR("Block signature is not aligned to block boundary; uri=", uri);
            return or_throw(yield, asio::error::invalid_argument);
        }

        auto block_digest = block_hash.close();

        e.block_digest = util::base64_encode(block_digest);

        // Encode the chained hash for the previous block.
        if (chain_hasher.prev_chained_digest())
            e.prev_chained_digest = util::base64_encode(*chain_hasher.prev_chained_digest());

        // Prepare hash for next data block: CHASH[i]=SHA2-512(CHASH[i-1] BLOCK[i])
        chain_hasher.calculate_block(ch.size, block_digest, *sig);

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
          , const AsioExecutor& ex, Cancel cancel, asio::yield_context yield)
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
fs::path
body_path_external( const fs::path& dirp
                  , const fs::path& cdirp
                  , sys::error_code& ec)
{
    fs::path body_path_p = dirp / body_path_fname;
    {
        auto body_path_s = fs::status(body_path_p, ec);
        if (!ec && !fs::is_regular_file(body_path_s))
            ec = asio::error::bad_descriptor;
        if (ec) return {};
    }

    auto body_cp_o = canonical_from_content_relpath(body_path_p, cdirp);
    if (!body_cp_o) {
        ec = asio::error::bad_descriptor;
        return {};
    }

    return *body_cp_o;
}

static
async_file_handle
open_body_external( const AsioExecutor& ex
                  , const fs::path& dirp
                  , const fs::path& cdirp
                  , sys::error_code& ec)
{
    auto body_cp = body_path_external(dirp, cdirp, ec);
    if (ec) return async_file_handle(ex);

    return util::file_io::open_readonly(ex, body_cp, ec);
}

static
std::size_t
body_size_external( const fs::path& dirp
                  , const fs::path& cdirp
                  , sys::error_code& ec)
{
    auto body_cp = body_path_external(dirp, cdirp, ec);
    if (ec) return 0;;

    return fs::file_size(body_cp, ec);
}

// `dirp` points to the `/.../data-vX` directory.
// `cdirp` may be set to a directory where an "external" resource body is
// searched for based on the content of `dirp`/`body_path_fname` file.
//
// TODO: It's not clear to me whether `cdirp` is actually being used in
// practice. It also seems very limiting to only have one `body_path_fname`
// reference. So find out whether that code path can be axed.
static
reader_uptr
_http_store_reader( const fs::path& dirp, boost::optional<const fs::path&> cdirp
                  , boost::optional<std::size_t> range_first
                  , boost::optional<std::size_t> range_last
                  , Cancel& cancel
                  , YieldContext yield)
{
    sys::error_code ec;
    assert(!cdirp || (fs::canonical(*cdirp, ec) == *cdirp));

    auto ex = yield.get_executor();

    // XXX: Actually the RFC7233 allows for range_last to be undefined
    // https://tools.ietf.org/html/rfc7233#section-2.1
    assert((!range_first && !range_last) || (range_first && range_last));

    auto headf = util::file_io::open_readonly(ex, dirp / head_fname, ec);
    if (ec) return or_throw<reader_uptr>(yield, ec);

    auto head = ResourceReader::read_signed_head(headf, cancel, yield.native());

    if (ec) {
        if (ec != asio::error::operation_aborted) {
            CACHE_RESOURCE_ERROR("Failed to parse stored response head");
        }
        return or_throw<reader_uptr>(yield, ec);
    }

    auto sigsf = util::file_io::open_readonly(ex, dirp / sigs_fname, ec);
    if (ec && ec != sys::errc::no_such_file_or_directory) return or_throw<reader_uptr>(yield, ec);
    ec = {};

    auto bodyf = util::file_io::open_readonly(ex, dirp / body_fname, ec);
    if (ec == sys::errc::no_such_file_or_directory && cdirp) {
        ec = {};
        bodyf = open_body_external(ex, dirp, *cdirp, ec);
    }
    if (ec && ec != sys::errc::no_such_file_or_directory) return or_throw<reader_uptr>(yield, ec);
    ec = {};

    boost::optional<Range> range;

    if (range_first) {
        // Check and convert range.
        assert(range_last);
        size_t begin = *range_first;
        size_t end   = *range_last + 1;
        if (begin > end) {
            _WARN("Inverted range boundaries: ", *range_first, " > ", *range_last);
            ec = sys::errc::make_error_code(sys::errc::invalid_seek);
            return or_throw<reader_uptr>(yield, ec);
        }
        if (!bodyf.is_open()) {
            if (begin > 0) {
                _WARN("Positive range requested for response with no stored data");
            }
            begin = 0;
            end = 0;
        } else {
            auto body_size = util::file_io::file_size(bodyf, ec);
            if (ec) return or_throw<reader_uptr>(yield, ec);
            if (begin > 0 &&  begin >= body_size) {
                _WARN( "Requested range 'first' goes beyond stored data: "
                     , util::HttpResponseByteRange{*range_first, *range_last, body_size});
                ec = sys::errc::make_error_code(sys::errc::invalid_seek);
                return or_throw<reader_uptr>(yield, ec);
            }
            // https://tools.ietf.org/html/rfc7233#section-2.1
            // Quote from the above link: If the last-byte-pos value is absent,
            // or if the value is greater than or equal to the current length
            // of the representation data, the byte range is interpreted as the
            // remainder of the representation (i.e., the server replaces the
            // value of last-byte-pos with a value that is one less than the
            // current length of the selected representation).
            end = std::min(end, body_size);
        }
        range = Range{begin, end};
    }

    return std::make_unique<ResourceReader>
        (std::move(head), std::move(sigsf), std::move(bodyf), range);
}

reader_uptr
http_store_reader( const fs::path& dirp, Cancel& cancel, YieldContext yield)
{
    return _http_store_reader
        (dirp, boost::none, {}, {}, cancel, yield);
}

reader_uptr
http_store_reader( const fs::path& dirp, const fs::path& cdirp, Cancel& cancel, YieldContext yield)
{
    return _http_store_reader
        (dirp, cdirp, {}, {}, cancel, yield);
}

reader_uptr
http_store_range_reader( const fs::path& dirp
                       , std::size_t first, std::size_t last
                       , Cancel& cancel, YieldContext yield)
{
    return _http_store_reader
        (dirp, boost::none, first, last, cancel, yield);
}

reader_uptr
http_store_range_reader( const fs::path& dirp, const fs::path& cdirp
                       , std::size_t first, std::size_t last
                       , Cancel& cancel, YieldContext yield)
{
    return _http_store_reader
        (dirp, cdirp, first, last, cancel, yield);
}

std::size_t
_http_store_body_size( const fs::path& dirp, boost::optional<const fs::path&> cdirp
                     , AsioExecutor ex
                     , sys::error_code& ec)
{
    namespace errc = sys::errc;
    assert(!cdirp || (fs::canonical(*cdirp, ec) == *cdirp));

    // At least the head file should exist,
    // otherwise opening the body file may fail
    // because the entry does not exist in the cache at all.
    if (!fs::exists(dirp / head_fname, ec)) {
        if (!ec) ec = errc::make_error_code(errc::no_such_file_or_directory);
        return 0;
    }

    auto bodysz = fs::file_size(dirp / body_fname, ec);
    if (!ec) return bodysz;
    if (ec != errc::no_such_file_or_directory) return 0;

    ec = asio::error::no_data;
    if (!cdirp) return 0;  // considered incomplete response

    ec = {};  // retry with content directory
    bodysz = body_size_external(dirp, *cdirp, ec);
    if (!ec) return bodysz;
    if (ec != errc::no_such_file_or_directory) return 0;

    ec = asio::error::no_data;
    return 0;  // also considered incomplete response
}

std::size_t
http_store_body_size( const fs::path& dirp, AsioExecutor ex
                    , sys::error_code& ec)
{
    return _http_store_body_size(dirp, boost::none, std::move(ex), ec);
}

std::size_t
http_store_body_size( const fs::path& dirp, const fs::path& cdirp, AsioExecutor ex
                    , sys::error_code& ec)
{
    return _http_store_body_size(dirp, cdirp, std::move(ex), ec);
}

fs::path
path_from_resource_id(fs::path dir, const ResourceId& resource_id)
{
    auto hex_digest = resource_id.hex_string();
    boost::string_view hd0(hex_digest); hd0.remove_suffix(hex_digest.size() - 2);
    boost::string_view hd1(hex_digest); hd1.remove_prefix(2);
    return dir.append(hd0.begin(), hd0.end()).append(hd1.begin(), hd1.end());
}

HashList
http_store_load_hash_list( const fs::path& dir
                         , AsioExecutor exec
                         , Cancel& cancel
                         , asio::yield_context yield)
{
    using Sha = util::SHA512;
    using Digest = Sha::digest_type;

    sys::error_code ec;

    auto headf = util::file_io::open_readonly(exec, dir / head_fname, ec);
    if (ec) return or_throw<HashList>(yield, ec);

    auto sigsf = util::file_io::open_readonly(exec, dir / sigs_fname, ec);
    if (ec) return or_throw<HashList>(yield, ec);

    HashList hl;

    hl.signed_head = ResourceReader::read_signed_head(headf, cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec, HashList{});

    std::string sig_buffer;

    while(true) {
        auto opt_sig_entry = SigEntry::parse(sigsf, sig_buffer, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, HashList{});

        if (!opt_sig_entry) break;

        auto d = util::base64_decode<Digest>(opt_sig_entry->block_digest);
        if (!d) return or_throw<HashList>(yield, asio::error::bad_descriptor);

        auto sig = util::base64_decode<Signature>(opt_sig_entry->signature);
        if (!sig) return or_throw<HashList>(yield, asio::error::bad_descriptor);

        hl.blocks.push_back({*d, *sig});
    }

    if (hl.blocks.empty()) {
        return or_throw<HashList>(yield, asio::error::not_found);
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

    reader_uptr
    reader(const ResourceId& resource_id, Cancel& cancel, YieldContext yield) override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        return http_store_reader(kpath, cancel, yield);
    }

    ReaderAndSize
    reader_and_size(const ResourceId& resource_id, Cancel& cancel, YieldContext yield) override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        sys::error_code ec;
        auto rr = http_store_reader(kpath, cancel, yield[ec]);
        if (ec) return or_throw<ReaderAndSize>(yield, ec);
        auto bs = http_store_body_size(kpath, executor, ec);
        if (ec) return or_throw<ReaderAndSize>(yield, ec);
        return {std::move(rr), bs};
    }

    reader_uptr
    range_reader(const ResourceId& resource_id, size_t first, size_t last, Cancel& cancel, YieldContext yield) override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        return http_store_range_reader(kpath, first, last, cancel, yield);
    }

    std::size_t
    body_size(const ResourceId& resource_id, sys::error_code& ec) const override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        return http_store_body_size(kpath, executor, ec);
    }

    std::size_t
    size(Cancel cancel, asio::yield_context yield) const override
    {
        // Do not use `for_each` since it can alter the store.
        sys::error_code ec;
        auto sz = recursive_dir_size(path, ec);
        ec = compute_error_code(ec, cancel);
        return or_throw(yield, ec, sz);
    }

    HashList
    load_hash_list(const ResourceId& resource_id, Cancel cancel, asio::yield_context yield) const override
    {
        auto dir = path_from_resource_id(path, resource_id);
        return http_store_load_hash_list(dir, executor, cancel, yield);
    }

protected:
    fs::path path;
    AsioExecutor executor;
};

class StaticHttpStore : public HttpReadStore {
public:
    StaticHttpStore(fs::path p, fs::path cp, util::Ed25519PublicKey pk, AsioExecutor ex)
        : HttpReadStore(std::move(p), std::move(ex))
        , content_path(std::move(cp)), verif_pubk(std::move(pk))
    {}

    ~StaticHttpStore() = default;

    reader_uptr
    reader(const ResourceId& resource_id, Cancel& cancel, YieldContext yield) override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        // Always verifying the response not only
        // protects the agent against malicions content in the static cache, it also
        // acts as a good citizen and avoids spreading such content to others.
        sys::error_code ec;
        auto rr = http_store_reader(kpath, content_path, cancel, yield[ec]);
        if (ec) return or_throw<reader_uptr>(yield, ec);
        return std::make_unique<VerifyingReader>(std::move(rr), verif_pubk);
    }

    ReaderAndSize
    reader_and_size(const ResourceId& resource_id, Cancel& cancel, YieldContext yield) override
    {
        sys::error_code ec;
        auto kpath = path_from_resource_id(path, resource_id);
        auto rr = std::make_unique<VerifyingReader>
            (http_store_reader(kpath, content_path, cancel, yield[ec]), verif_pubk);
        if (ec) return or_throw<ReaderAndSize>(yield, ec);
        auto bs = http_store_body_size(kpath, content_path, executor, ec);
        if (ec) return or_throw<ReaderAndSize>(yield, ec);
        return {std::move(rr), bs};
    }

    reader_uptr
    range_reader(const ResourceId& resource_id, size_t first, size_t last, Cancel& cancel, YieldContext yield) override
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
        return http_store_range_reader(kpath, content_path, first, last, cancel, yield);
    }

    std::size_t
    body_size(const ResourceId& resource_id, sys::error_code& ec) const override
    {
        auto kpath = path_from_resource_id(path, resource_id);
        return http_store_body_size(kpath, content_path, executor, ec);
    }

    std::size_t
    size(Cancel cancel, asio::yield_context yield) const override
    {
        sys::error_code ec;
        auto sz = HttpReadStore::size(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, 0);
        sz += recursive_dir_size(content_path, ec);
        ec = compute_error_code(ec, cancel);
        return or_throw(yield, ec, sz);
    }

private:
    fs::path content_path;
    util::Ed25519PublicKey verif_pubk;
};

std::unique_ptr<BaseHttpStore>
make_static_http_store( fs::path path, fs::path content_path
                      , util::Ed25519PublicKey pk, AsioExecutor ex)
{
    using namespace std;
    return make_unique<StaticHttpStore>(move(path), move(content_path), move(pk), move(ex));
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

    void
    for_each(keep_func, Cancel, YieldContext) override;

    void
    store( const ResourceId& resource_id, http_response::AbstractReader&
         , Cancel, asio::yield_context) override;

    reader_uptr
    reader(const ResourceId& resource_id, Cancel& cancel, YieldContext yield) override
    { return read_store->reader(resource_id, cancel, yield); }

    ReaderAndSize
    reader_and_size(const ResourceId& resource_id, Cancel& cancel, YieldContext yield) override
    { return read_store->reader_and_size(resource_id, cancel, yield); }

    reader_uptr
    range_reader(const ResourceId& resource_id, size_t first, size_t last, Cancel& cancel, YieldContext yield) override
    { return read_store->range_reader(resource_id, first, last, cancel, yield); }

    std::size_t
    body_size(const ResourceId& resource_id, sys::error_code& ec) const override
    { return read_store->body_size(resource_id, ec); }

    std::size_t
    size(Cancel cancel, asio::yield_context ec) const override
    { return read_store->size(cancel, ec); }

    HashList
    load_hash_list(const ResourceId& resource_id, Cancel cancel, asio::yield_context yield) const override
    { return read_store->load_hash_list(resource_id, cancel, yield); }

protected:
    fs::path path;
    AsioExecutor executor;
    std::unique_ptr<BaseHttpStore> read_store;
};

void
FullHttpStore::for_each( keep_func keep
                       , Cancel cancel, YieldContext yield)
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

            auto rr = http_store_reader(p, cancel, yield[ec]);
            if (ec) {
               _WARN("Failed to open cached response: ", p, "; ec=", ec);
               try_remove(p); continue;
            }
            assert(rr);

            auto keep_entry = keep(*resource_id, std::move(rr), yield.native()[ec]);
            ec = compute_error_code(ec, cancel);
            if (ec == asio::error::operation_aborted) return or_throw(yield, ec);
            if (ec) {
                _WARN("Failed to check cached response: ", p, "; ec=", ec);
                try_remove(p); continue;
            }

            if (!keep_entry)
                try_remove(p);
        }
    }
}

void
FullHttpStore::store( const ResourceId& resource_id, http_response::AbstractReader& r
                    , Cancel cancel, asio::yield_context yield)
{
    sys::error_code ec;

    auto kpath = path_from_resource_id(path, resource_id);

    auto kpath_parent = kpath.parent_path();
    fs::create_directory(kpath_parent, ec);
    if (ec) return or_throw(yield, ec);

    // Replacing a directory is not an atomic operation,
    // so try to remove the existing entry before committing.
    auto dir = util::atomic_dir::make(kpath, ec);
    if (!ec) http_store(r, dir->temp_path(), executor, cancel, yield[ec]);
    if (!ec && fs::exists(kpath)) fs::remove_all(kpath, ec);
    // A new version of the response may still slip in here,
    // but it may be ok since it will probably be recent enough.
    if (!ec) dir->commit(ec);
    if (!ec) _DEBUG("Stored to directory; resource_id=", resource_id, " path=", kpath);
    else _ERROR( "Failed to store response; resource_id=", resource_id, " path=", kpath
               , " ec=", ec);
    return or_throw(yield, ec);
}

std::unique_ptr<HttpStore>
make_http_store(fs::path path, AsioExecutor ex)
{
    using namespace std;
    auto read_store = make_unique<HttpReadStore>(path, ex);
    return make_unique<FullHttpStore>(move(path), move(ex), move(read_store));
}

class BackedHttpStore : public FullHttpStore {
public:
    BackedHttpStore( fs::path p, AsioExecutor ex
                   , std::unique_ptr<BaseHttpStore> rs, std::unique_ptr<BaseHttpStore> fs)
        : FullHttpStore(std::move(p), std::move(ex), std::move(rs))
        , fallback_store(std::move(fs))
    {}

    ~BackedHttpStore() = default;

    reader_uptr
    reader(const ResourceId& resource_id, Cancel& cancel, YieldContext yield) override
    {
        sys::error_code ec;
        auto ret = FullHttpStore::reader(resource_id, cancel, yield[ec]);
        if (!ec) return ret;
        _DEBUG("Failed to create reader for resource_id, trying fallback store: ", resource_id);
        return fallback_store->reader(resource_id, cancel, yield);
    }

    ReaderAndSize
    reader_and_size(const ResourceId& resource_id, Cancel& cancel, YieldContext yield) override
    {
        sys::error_code ec;
        auto ret = FullHttpStore::reader_and_size(resource_id, cancel, yield[ec]);
        if (!ec) return ret;
        _DEBUG("Failed to create reader for resource_id, trying fallback store: ", resource_id);
        return fallback_store->reader_and_size(resource_id, cancel, yield);
    }

    reader_uptr
    range_reader(const ResourceId& resource_id, size_t first, size_t last, Cancel& cancel, YieldContext yield) override
    {
        sys::error_code ec;
        auto ret = FullHttpStore::range_reader(resource_id, first, last, cancel, yield[ec]);
        if (!ec) return ret;
        _DEBUG("Failed to create range reader for resource_id, trying fallback store: ", resource_id);
        return fallback_store->range_reader(resource_id, first, last, cancel, yield);
    }

    std::size_t
    body_size(const ResourceId& resource_id, sys::error_code& ec) const override
    {
        auto ret = FullHttpStore::body_size(resource_id, ec);
        if (!ec) return ret;
        _DEBUG("Failed to get body size for resource_id, trying fallback store: ", resource_id);
        return fallback_store->body_size(resource_id, ec = {});
    }

    std::size_t
    size(Cancel cancel, asio::yield_context yield) const override
    {
        sys::error_code ec;
        auto sz1 = FullHttpStore::size(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, 0);
        auto sz2 = fallback_store->size(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec, 0);
        return sz1 + sz2;
    }

    HashList
    load_hash_list(const ResourceId& resource_id, Cancel cancel, asio::yield_context yield) const override
    {
        sys::error_code ec;
        auto ret = FullHttpStore::load_hash_list(resource_id, cancel, yield[ec]);
        if (!ec) return ret;
        if (cancel) return or_throw<HashList>(yield, asio::error::operation_aborted);
        _DEBUG("Failed to load hash list for resource_id, trying fallback store: ", resource_id);
        return fallback_store->load_hash_list(resource_id, cancel, yield);
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
    return make_unique<BackedHttpStore>( move(path), move(ex)
                                       , move(read_store), move(fallback_store));
}

}} // namespaces
