#pragma once

#include <boost/asio/executor.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem/path.hpp>

#include "../response_reader.h"
#include "../util/signal.h"

#include "../namespaces.h"

namespace ouinet { namespace cache {

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

}} // namespaces
