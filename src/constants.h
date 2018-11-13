#pragma once

#include <boost/config.hpp>

#define OUINET_CLIENT_SERVER_STRING   "Ouinet.Client"
#define OUINET_INJECTOR_SERVER_STRING "Ouinet.Injector"

namespace ouinet { namespace http_ {
// TODO: This should be called ``http``,
// but it is already being used as an alias for ``boost::http``.

// Common prefix for all Ouinet-specific internal HTTP headers.
static const std::string header_prefix = "X-Ouinet-";

// The presence of this (non-empty) HTTP request header
// shows the protocol version used by the client
// and hints the receiving injector to behave like an injector instead of a proxy.
static const std::string request_version_hdr = header_prefix + "Version";
static const std::string request_version_hdr_v0 = "0";
static const std::string request_version_hdr_v1 = "1";
static const std::string request_version_hdr_current = request_version_hdr_v1;

// Such a request should get the following HTTP response header
// with an opaque identifier for this insertion.
static const std::string response_injection_id_hdr = header_prefix + "Injection-ID";

// The presence of this HTTP request header with the true value below
// instructs the injector to behave synchronously
// and inline the resulting descriptor in response headers.
static const std::string request_sync_injection_hdr = header_prefix + "Sync";
static const std::string request_sync_injection_true = "true";

// If synchronous injection is enabled in an HTTP request,
// this header is added to the resulting response
// with the Base64-encoded, Zlib-compressed content of the descriptor.
static const std::string response_descriptor_hdr = header_prefix + "Descriptor";

// Also, this is added with a link to descriptor storage.
static const std::string response_descriptor_link_hdr = header_prefix + "Descriptor-Link";

// Also, this is added with Base64-encoded, db-specific data to help reinsert.
// The name of the db is added at the end of the header name.
static const std::string response_insert_hdr_pfx = header_prefix + "Insert-";

static const std::string response_error_hdr = header_prefix + "Error";

}} // namespaces
