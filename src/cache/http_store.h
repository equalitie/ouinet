#pragma once

#include <boost/asio/executor.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem/path.hpp>

#include "../response_reader.h"
#include "../util/signal.h"

#include "../namespaces.h"

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

// Save the HTTP response coming from the given reader into the given directory,
// following the storage format in `http_store_version`.
//
// The response is assumed to have valid HTTP signatures,
// otherwise storage will fail.
//
// The directory must already exist and be writable.
// Trying to overwrite existing files will cause an error.
void http_store( http_response::AbstractReader&, const fs::path&
               , const asio::executor&, Cancel, asio::yield_context);

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

}} // namespaces
