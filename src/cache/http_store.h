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

// This format splits individual HTTP responses into the following files:
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
static const unsigned http_store_version = 1;

//// Low-level functions for HTTP response storage:

// Save the HTTP response coming from the given reader in v0 format
// into the given open stream.
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
void http_store_v1( http_response::AbstractReader&, const fs::path&
                  , const asio::executor&, Cancel, asio::yield_context);

// Return a new reader for a response stored in v0 format
// in the given file.
std::unique_ptr<http_response::AbstractReader>
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
std::unique_ptr<http_response::AbstractReader>
http_store_reader_v1( fs::path, asio::executor
                    , sys::error_code&);

//// High-level classes for HTTP response storage

class AbstractHttpStore {
public:
    using keep_func = std::function<
        bool(http_response::AbstractReader&, asio::yield_context)>;

public:
    virtual ~AbstractHttpStore() = default;

    // Iterate over stored responses
    // and keep those for which invoking the `keep_func` returns true.
    virtual
    void
    keep_if(keep_func, asio::yield_context) = 0;

    virtual
    void
    store( const std::string& key, http_response::AbstractReader&
         , Cancel, asio::yield_context) = 0;

    virtual
    std::unique_ptr<http_response::AbstractReader>
    reader( const std::string& key
          , sys::error_code&) = 0;  // not const, e.g. LRU cache
};

// This uses format v0 to store each response
// in a file named `LOWER_HEX(SHA1(KEY))`
// under the given directory.
class HttpStoreV0 : public AbstractHttpStore {
public:
    HttpStoreV0(fs::path, asio::executor ex)
        : path(std::move(path)), executor(ex)
    {}

    ~HttpStoreV0() override;

    void
    keep_if(keep_func, asio::yield_context) override;

    void
    store( const std::string& key, http_response::AbstractReader&
         , Cancel, asio::yield_context) override;

    std::unique_ptr<http_response::AbstractReader>
    reader( const std::string& key
          , sys::error_code&) override;

private:
    fs::path path;
    asio::executor executor;
};

}} // namespaces
