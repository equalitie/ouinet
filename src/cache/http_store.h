#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/filesystem/path.hpp>

#include "../util/signal.h"
#include "../session.h"

#include "../namespaces.h"

namespace ouinet { namespace cache {

static const unsigned http_store_version = 1;

// Save the HTTP response in the given session into the given directory,
// following the storage format in `http_store_version`.
//
// The response is assumed to have valid HTTP signatures,
// otherwise storage will fail.
//
// The directory must already exist and be writable.
// Trying to overwrite existing files will cause an error.
void http_store( Session&, const fs::path&
               , Cancel, asio::yield_context);

}} // namespaces
