#pragma once

#include <functional>

#include <boost/asio/executor.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem/path.hpp>

#include "hash_list.h"
#include "../constants.h"
#include "../response_reader.h"
#include "../util/crypto.h"
#include "../util/signal.h"

#include "../namespaces.h"

namespace ouinet { namespace cache {

// When a client gets a `HEAD` request for a URL,
// this response header indicates the data range that it can send back
// (either for a full or range request).
//
// The format is the same one used in `Content-Range` headers
// (RFC7233#4.2).
static const std::string response_available_data = http_::header_prefix + "Avail-Data";

using reader_uptr = std::unique_ptr<http_response::AbstractReader>;

// Save the HTTP response coming from the given reader into the given
// directory.
//
// The response is assumed to have valid HTTP signatures,
// otherwise storage will fail.
//
// The directory must already exist and be writable.
// Trying to overwrite existing files will cause an error.
//
// ----
//
// The format splits individual HTTP responses into the following files:
//
//   - `head`: It contains the raw head of the response (terminated by CRLF,
//     with headers also CRLF-terminated), but devoid of framing headers
//     (i.e. `Content-Length`, `Transfer-Encoding` and `Trailers`).  When the
//     whole response has been successfully read, trailers are appended as
//     normal headers, with redundant signatures removed.
//
//   - `body`: This is the raw body data (flat, no chunking or other framing).
//
//   - `sigs`: This contains block signatures and chained hashes.  It consists of
//     fixed length, LF-terminated lines with the following format
//     for blocks i=0,1...:
//
//         PAD016_LHEX(OFFSET[i])<SP>BASE64(SIG[i])<SP>BASE64(DHASH[i])<SP>BASE64(CHASH[i-1])
//
//     Where `PAD016_LHEX(x)` represents `x` in lower-case hexadecimal, zero-padded to 16 characters,
//     `BASE64(CHASH[-1])` is established as `BASE64('\0' * 64)` (for padding the first line),
//     `SIG[-1]` and `CHASH[-1]` are established as the empty string (for `CHASH[0]` computation),
//     `DHASH[i]=SHA2-512(DATA[i])` (block data hash)
//     `CHASH[i]=SHA2-512(SIG[i-1] CHASH[i-1] DHASH[i])` (block chain hash).
//
// Some reading functions below allow specifying a *content directory* that
// holds an arbitrary hierarchy containing body data files
// outside of the directory storing the response.
// In this case, the later directory may include a `body-path` file which
// contains the path of the body data file relative to the content root path,
// with forward slashes as path separators, without `.` or `..` components,
// and without a final new line.
// Such responses need to be stored using external tools.
void http_store( http_response::AbstractReader&, const fs::path&
               , const asio::executor&, Cancel, asio::yield_context);
// TODO: This format is both inefficient for multi-peer downloads (Base64 decoding needed)
// and inadequate for partial responses (`ouipsig` is in previous `sigs` file line, maybe missing).
// A format with binary records or just SIG/DHASH/CHASH of the *current* block might be more convenient
// (DHASH may be zero in the first record).

// Return a new reader for a response under the given directory `dirp`.
//
// At least the file belonging to the response head must be readable,
// otherwise the call will report an error and not return a reader.
// If other pieces are missing, the reader may fail further down the road.
//
// The response will be provided using chunked transfer encoding,
// with all the metadata needed to verify and further share it.
//
// An incomplete response will work as usual,
// but cause `boost::asio::error::connection_aborted` ("Software caused connection abort")
// when no more body data is available.
// To detect such cases beforehand, use `http_store_body_size`.
reader_uptr
http_store_reader(const fs::path& dirp, asio::executor, sys::error_code&);

// Same as above, but get body data from files stored in the given content directory `cdirp`.
//
// The files for the response under `dirp` should either contain the body data itself
// or otherwise point to the path of the data file relative to `cdirp`.
//
// Warning: Although security checks are performed on the pointers to body data files
// (e.g. to check that they are not outside of the content directory),
// none are performed on `cdirp` itself.
// Please make sure that `cdirp` is already in canonical form or some checks may fail.
reader_uptr
http_store_reader( const fs::path& dirp, const fs::path& cdirp
                 , asio::executor, sys::error_code&);

// Same as above, but allow specifying a contiguous range of data to read
// instead of the whole response.
//
// The partial response will have the HTTP status `206 Partial Content`,
// with the original HTTP status code in the `X-Ouinet-HTTP-Status` header
// and a `Content-Range` header.
//
// `first` and `last` follow RFC7233#2.1 notation:
// `first` must be strictly less than total data size;
// `last` must be at least `first` and strictly less than total data size.
// Open ranges ("N-" and "-N") are not supported.
//
// If the range would cover data which is not stored,
// a `boost::system::errc::invalid_seek` error is reported
// (which may be interpreted as HTTP status `416 Range Not Satisfiable`).
reader_uptr
http_store_range_reader( const fs::path& dirp, asio::executor
                       , std::size_t first, std::size_t last
                       , sys::error_code&);

// Same as above, but get body data from files stored in the given content directory `cdirp`.
//
// The files for the response under `dirp` should either contain the body data itself
// or otherwise point to the path of the data file relative to `cdirp`.
//
// Warning: Although security checks are performed on the pointers to body data files
// (e.g. to check that they are not outside of the content directory),
// none are performed on `cdirp` itself.
// Please make sure that `cdirp` is already in canonical form or some checks may fail.
reader_uptr
http_store_range_reader( const fs::path& dirp, const fs::path& cdirp, asio::executor
                       , std::size_t first, std::size_t last
                       , sys::error_code&);

// Return the size of body data currently stored for a response under the given directory `dirp`.
//
// For an incomplete respone, this may be less than the size claimed in its head.
//
// If the response does not exist in the store,
// a `sys::errc::no_such_file_or_directory` error is reported.
std::size_t
http_store_body_size( const fs::path& dirp, asio::executor
                    , sys::error_code&);

// Same as above, but get body data from files stored in the given content directory `cdirp`.
//
// The files for the response under `dirp` should either contain the body data itself
// or otherwise point to the path of the data file relative to `cdirp`.
//
// Warning: Although security checks are performed on the pointers to body data files
// (e.g. to check that they are not outside of the content directory),
// none are performed on `cdirp` itself.
// Please make sure that `cdirp` is already in canonical form or some checks may fail.
std::size_t
http_store_body_size( const fs::path& dirp, const fs::path& cdirp, asio::executor
                    , sys::error_code&);

HashList
http_store_load_hash_list(const fs::path&, asio::executor, Cancel&, asio::yield_context);

//// High-level classes for HTTP response storage

// Store each response in a directory named `DIGEST[:2]/DIGEST[2:]` (where
// `DIGEST = LOWER_HEX(SHA1(KEY))`) under the given directory.
class BaseHttpStore {
public:
    virtual ~BaseHttpStore() = default;

    virtual reader_uptr
    reader(const std::string& key, sys::error_code&) = 0;

    virtual reader_uptr
    range_reader(const std::string& key, size_t first, size_t last, sys::error_code&) = 0;

    virtual std::size_t
    body_size(const std::string& key, sys::error_code&) const = 0;

    virtual std::size_t
    size(Cancel, asio::yield_context) const = 0;

    virtual HashList
    load_hash_list(const std::string& key, Cancel, asio::yield_context) const = 0;
};

// As static HTTP stores may come from untrusted sources,
// an HTTP signing key is needed for verification of any response coming out of the store,
// i.e. a reader constructed by this store will cryptographically verify its response.
//
// Warning: Due to implementation limitations,
// this verification is currently not performed by range readers.
//
// Warning: Although security checks are performed on the pointers to body data files
// (e.g. to check that they are not outside of the content directory),
// none are performed on `content_path` itself.
// Please make sure that `content_path` is already in canonical form or some checks may fail.
std::unique_ptr<BaseHttpStore>
make_static_http_store( fs::path path, fs::path content_path
                      , util::Ed25519PublicKey
                      , asio::executor);

class HttpStore : public BaseHttpStore {
public:
    using keep_func = std::function<
        bool(reader_uptr, asio::yield_context)>;

public:
    virtual ~HttpStore() = default;

    virtual void
    for_each(keep_func, Cancel, asio::yield_context) = 0;

    virtual void
    store( const std::string& key, http_response::AbstractReader&
         , Cancel, asio::yield_context) = 0;
};

std::unique_ptr<HttpStore>
make_http_store(fs::path path, asio::executor);

std::unique_ptr<HttpStore>
make_backed_http_store( fs::path path, std::unique_ptr<BaseHttpStore> fallback_store
                      , asio::executor);

}} // namespaces
