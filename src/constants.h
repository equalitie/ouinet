#pragma once

#include <boost/config.hpp>
#include <boost/regex.hpp>

#define OUINET_CLIENT_SERVER_STRING   "Ouinet.Client"
#define OUINET_INJECTOR_SERVER_STRING "Ouinet.Injector"

namespace ouinet { namespace http_ {
// TODO: This should be called ``http``,
// but it is already being used as an alias for ``boost::http``.

// Common prefix for all Ouinet-specific internal HTTP headers.
static const std::string header_prefix = "X-Ouinet-";


// Version-independent headers:

// The presence of this (non-empty) HTTP request header
// shows the protocol version used by the client
// and hints the receiving injector to behave like an injector instead of a proxy.
//
// Such a request should get the following HTTP response header
// indicating the protocol version used by the injector.
//
// The format of this header is guaranteed to be `[0-9]+`
// for all versions of the protocol (including future ones).
static const std::string protocol_version_hdr = header_prefix + "Version";
static const boost::regex protocol_version_rx("^([0-9]+)$");

static const std::string protocol_version_hdr_v0 = "0";
static const std::string protocol_version_hdr_v1 = "1";
static const std::string protocol_version_hdr_v2 = "2";
static const std::string protocol_version_hdr_v3 = "3";
static const std::string protocol_version_hdr_v4 = "4";
static const std::string protocol_version_hdr_v5 = "5";
static const std::string protocol_version_hdr_current = protocol_version_hdr_v5;
static const unsigned protocol_version_current = 5;

// The presence of this HTTP request header
// indicates that an error happened processing the request,
// with informatio complementing the HTTP status code.
//
// The format of this header is guaranteed to be `[0-9]+ [\x21-\x7E][\x20-\x7E]*`
// for all versions of the protocol (including future ones).
//
// This means that, for any request with any value of `X-Ouinet-Version`
// (even newer than those accepted by the receiver),
// a response with just the same `X-Ouinet-Version` and an `X-Ouinet-Error`
// shall always be accepted.
static const std::string response_error_hdr = header_prefix + "Error";
static const boost::regex response_error_rx("^([0-9]+) ([\\x21-\\x7E][\\x20-\\x7E]*)$");

// Internal error codes.
static const std::string response_error_hdr_version_too_low  = "1 Client's version too low";
static const std::string response_error_hdr_version_too_high = "2 Client's version too high";
static const std::string response_error_hdr_retrieval_failed = "3 Resource retrieval failed";


// Version-dependent headers:

static const std::string response_warning_hdr = header_prefix + "Warning";

// This allows the response to stand on its own (e.g. for reinsertion).
static const std::string response_uri_hdr = header_prefix + "URI";
// This contains identifying data about the injection itself.
static const std::string response_injection_hdr = header_prefix + "Injection";

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


// Other headers (e.g. agent-only):

// This indicates what mechanism is the source of this response.
// It can be used by the agent to style its representation.
static const std::string response_source_hdr = header_prefix + "Source";
// Values for the header above.
static const std::string response_source_hdr_front_end = "front-end";
static const std::string response_source_hdr_origin = "origin";
static const std::string response_source_hdr_proxy = "proxy";
static const std::string response_source_hdr_injector = "injector";
static const std::string response_source_hdr_dist_cache = "dist-cache";
static const std::string response_source_hdr_local_cache = "local-cache";

} // http_ namespace

static const uint16_t default_udp_port = 28729;

} // ouinet namespace
