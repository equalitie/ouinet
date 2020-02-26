#pragma once

#include <functional>

#include <boost/asio/executor.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem/path.hpp>

#include "../response_reader.h"
#include "../util/signal.h"

#include "../namespaces.h"

#include "detail/http_store.h"

namespace ouinet { namespace cache {

using reader_uptr = std::unique_ptr<http_response::AbstractReader>;

//// Low-level functions for HTTP response storage:

// Save the HTTP response coming from the given reader in v0 format
// into the given open stream.
//
// ----
//
// The v0 format is just a raw dump of the whole HTTP response
// (head, body and trailer) as it comes from the sender.
template<class Stream>
void http_store_v0( http_response::AbstractReader& reader, Stream& outf
                  , Cancel cancel, asio::yield_context yield)
{
    detail::http_store_v0(reader, outf, cancel, yield);
}

// Save the HTTP response coming from the given reader in v1 format
// into the given directory.
//
// The response is assumed to have valid HTTP signatures,
// otherwise storage will fail.
//
// The directory must already exist and be writable.
// Trying to overwrite existing files will cause an error.
//
// ----
//
// The v1 format splits individual HTTP responses into the following files:
//
//   - `head`: It contains the raw head of the response (terminated by CRLF,
//     with headers also CRLF-terminated), but devoid of framing headers
//     (i.e. `Content-Length`, `Transfer-Encoding` and `Trailers`).  When the
//     whole response has been successfully read, trailers are appended as
//     normal headers, with redundant signatures removed.
//
//   - `body`: This is the raw body data (flat, no chunking or other framing).
//
//   - `sigs`: This contains block signatures and chained hashes.  It consists
//     of LF-terminated lines with the following format for blocks i=0,1...:
//
//         LOWER_HEX(OFFSET[i])<SP>BASE64(SIG[i])<SP>BASE64(HASH([i-1]))
//
//     Where `BASE64(HASH[-1])` and `HASH[-1]` are the empty string and
//     `HASH[i]=HASH(HASH[i-1] DATA[i])`.
//
void http_store_v1( http_response::AbstractReader&, const fs::path&
                  , const asio::executor&, Cancel, asio::yield_context);

// Return a new reader for a response stored in v0 format
// in the given file.
reader_uptr
http_store_reader_v0( const fs::path&, asio::executor
                    , sys::error_code&);

// Return a new reader for a response stored in v1 format
// under the given directory.
//
// Both the path and the executor are kept by the reader.
//
// At least the file belonging to the response head must be readable,
// otherwise the call will report an error and not return a reader.
// If other pieces are missing, the reader may fail further down the road.
//
// The response will be provided using chunked transfer encoding,
// with all the metadata needed to verify and further share it.
reader_uptr
http_store_reader_v1( fs::path, asio::executor
                    , sys::error_code&);

// Same as above, but allow specifying a contiguous range of data to read
// instead of the whole response.
//
// The partial response will have the status `206 Partial Content`,
// with the original HTTP status code in the `X-Ouinet-HTTP-Status` header
// and a `Content-Range` header.
//
// `pos` must be strictly less than total data size,
// and `len` must be at least 1.
// `pos + len` must not be greater than total data size.
// Please note that these differ from RFC7233#2.1 "first-last" notation.
//
// If the range would cover data which is not stored,
// a `416 Range Not Satisfiable` error is reported.
reader_uptr
http_store_reader_v1( fs::path, asio::executor
                    , std::size_t pos, std::size_t len
                    , sys::error_code&);

//// High-level classes for HTTP response storage

class AbstractHttpStore {
public:
    using keep_func = std::function<
        bool(reader_uptr, asio::yield_context)>;

public:
    virtual ~AbstractHttpStore() = default;

    // Iterate over stored responses.
    //
    // `keep_func` gets an open reader for the response;
    // the response is removed if `keep_func` returns false
    // (and there is no error).
    //
    // Other maintenance may be performed too.
    virtual
    void
    for_each(keep_func, asio::yield_context) = 0;

    virtual
    void
    store( const std::string& key, http_response::AbstractReader&
         , Cancel, asio::yield_context) = 0;

    virtual
    reader_uptr
    reader( const std::string& key
          , sys::error_code&) = 0;  // not const, e.g. LRU cache
};

// This uses format v0 to store each response
// in a file named `LOWER_HEX(SHA1(KEY))`
// under the given directory.
class HttpStoreV0 : public AbstractHttpStore {
public:
    HttpStoreV0(fs::path p, asio::executor ex)
        : path(std::move(p)), executor(ex)
    {}

    ~HttpStoreV0() override;

    void
    for_each(keep_func, asio::yield_context) override;

    void
    store( const std::string& key, http_response::AbstractReader&
         , Cancel, asio::yield_context) override;

    reader_uptr
    reader( const std::string& key
          , sys::error_code&) override;

private:
    fs::path path;
    asio::executor executor;
};

// This uses format v1 to store each response
// in a directory named `DIGEST[:2]/DIGEST[2:]`
// (where `DIGEST = LOWER_HEX(SHA1(KEY))`)
// under the given directory.
class HttpStoreV1 : public AbstractHttpStore {
public:
    HttpStoreV1(fs::path p, asio::executor ex)
        : path(std::move(p)), executor(ex)
    {}

    ~HttpStoreV1() override;

    void
    for_each(keep_func, asio::yield_context) override;

    void
    store( const std::string& key, http_response::AbstractReader&
         , Cancel, asio::yield_context) override;

    reader_uptr
    reader( const std::string& key
          , sys::error_code&) override;

private:
    fs::path path;
    asio::executor executor;
};

}} // namespaces
