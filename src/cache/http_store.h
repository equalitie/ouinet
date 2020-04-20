#pragma once

#include <functional>

#include <boost/asio/executor.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem/path.hpp>

#include "../constants.h"
#include "../response_reader.h"
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
//   - `sigs`: This contains block signatures and chained hashes.  It consists
//     of LF-terminated lines with the following format for blocks i=0,1...:
//
//         LOWER_HEX(OFFSET[i])<SP>BASE64(SIG[i])<SP>BASE64(HASH([i-1]))
//
//     Where `BASE64(HASH[-1])` and `HASH[-1]` are the empty string and
//     `HASH[i]=HASH(HASH[i-1] DATA[i])`.
//
void http_store( http_response::AbstractReader&, const fs::path&
               , const asio::executor&, Cancel, asio::yield_context);

// Return a new reader for a response under the given directory.
//
// At least the file belonging to the response head must be readable,
// otherwise the call will report an error and not return a reader.
// If other pieces are missing, the reader may fail further down the road.
//
// The response will be provided using chunked transfer encoding,
// with all the metadata needed to verify and further share it.
reader_uptr
http_store_reader( const fs::path&, asio::executor, sys::error_code&);

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
http_store_range_reader( const fs::path&, asio::executor
                       , std::size_t first, std::size_t last
                       , sys::error_code&);

// Same as above, but return a reader that only yields the response head,
// as if an HTTP `HEAD` request was performed.
//
// The head will contain an `X-Ouinet-Avail-Data` header
// showing the available stored data in the same format as
// the `Content-Range` header (see RFC7233#4.2).
reader_uptr
http_store_head_reader( const fs::path&, asio::executor
                      , sys::error_code&);

//// High-level classes for HTTP response storage

// Store each response in a directory named `DIGEST[:2]/DIGEST[2:]` (where
// `DIGEST = LOWER_HEX(SHA1(KEY))`) under the given directory.
class HttpStore {
public:
    using keep_func = std::function<
        bool(reader_uptr, asio::yield_context)>;

public:
    HttpStore(fs::path p, asio::executor ex)
        : path(std::move(p)), executor(ex)
    {}

    ~HttpStore();

    void
    for_each(keep_func, Cancel, asio::yield_context);

    void
    store( const std::string& key, http_response::AbstractReader&
         , Cancel, asio::yield_context);

    reader_uptr
    reader(const std::string& key, sys::error_code&);

    std::size_t
    size(Cancel, asio::yield_context) const;

private:
    fs::path path;
    asio::executor executor;
};

}} // namespaces
