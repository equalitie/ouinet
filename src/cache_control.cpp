#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>

#include "cache_control.h"
#include "or_throw.h"
#include "split_string.h"
#include "util.h"

using namespace std;
using namespace ouinet;

using string_view = beast::string_view;
using Request = CacheControl::Request;
using Response = CacheControl::Response;
using Request = CacheControl::Request;
using boost::optional;

namespace posix_time = boost::posix_time;

// Look for a literal directive (like "no-cache" but not "max-age=N")
// in the "Cache-Control" header field
// of a request or response.
template <bool isRequest, class Body>
static
bool has_cache_control_directive( const http::message<isRequest, Body>& request
                                , const string_view& directive)
{
    auto cache_control_i = request.find(http::field::cache_control);
    if (cache_control_i == request.end()) return false;

    for (auto kv : SplitString(cache_control_i->value(), ',')) {
        if (boost::iequals(kv, directive)) return true;
    }

    return false;
}

template<class R>
static optional<string_view> get(const R& r, http::field f)
{
    auto i = r.find(f);
    if (i == r.end()) return boost::none;
    return i->value();
}

static
optional<unsigned> get_max_age(const string_view& cache_control_value)
{
    using boost::optional;

    optional<unsigned> max_age;
    optional<unsigned> s_maxage;

    auto update_max_age = [] (optional<unsigned>& max_age, string_view value) {
        unsigned delta = util::parse_num<unsigned>(value, unsigned(-1));

        // TODO: What does RFC say about malformed entries?
        if (delta == unsigned(-1)) return;

        if (!max_age || *max_age < delta) {
            max_age = delta;
        }
    };

    for (auto kv : SplitString(cache_control_value, ',')) {
        beast::string_view key, val;
        std::tie(key, val) = split_string_pair(kv, '=');

        if (boost::iequals(key, "s-maxage")) {
            update_max_age(s_maxage, val);
        }

        if (boost::iequals(key, "max-age")) {
            update_max_age(max_age, val);
        }
    }

    if (s_maxage) return s_maxage;
    return max_age;
}

static
bool is_expired(const CacheControl::CacheEntry& entry)
{
    using boost::optional;

    // RFC2616: https://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html#sec14.9.3

    // TODO: Also check the HTTP/1.0 'Expires' header field.

    auto cache_control_value = get(entry.response, http::field::cache_control);

    if (!cache_control_value) {
        return true;
    }

    optional<unsigned> max_age = get_max_age(*cache_control_value);
    if (!max_age) return true;

    auto now = posix_time::second_clock::universal_time();

    return now > entry.time_stamp + posix_time::seconds(*max_age);
}

bool
CacheControl::is_older_than_max_cache_age(const posix_time::ptime& time_stamp) const
{
    static const posix_time::time_duration never_expired = posix_time::seconds(-1);
    auto now = posix_time::second_clock::universal_time();

    if (_max_cached_age != never_expired && now - time_stamp > _max_cached_age) {
        return true;
    }

    return false;
}

static
Response add_warning(Response response, const char* value)
{
    response.set(http::field::warning, value);
    return response;
}

static
Response add_stale_warning(Response response)
{
    return add_warning( move(response)
                      , "110 Ouinet 'Response is stale'");
}

static bool has_correct_content_length(const Response& rs)
{
    // Relevant RFC https://tools.ietf.org/html/rfc7230#section-3.3.2
    auto opt_length = get(rs, http::field::content_length);
    if (!opt_length) return true;
    auto length = util::parse_num<size_t>(*opt_length, size_t(-1));
    if (length == size_t(-1)) return false;
    return rs.body().size() == length;
}

Response
CacheControl::fetch(const Request& request, asio::yield_context yield)
{
    sys::error_code ec;
    auto response = do_fetch(request, yield[ec]);

    if(!ec && !has_correct_content_length(response)) {
        cerr << "----- WARNING: Incorrect content length ----" << endl;
        cerr << request << response;
        cerr << "--------------------------------------------" << endl;
    }

    return or_throw(yield, ec, move(response));
}

// TODO: This function is unfinished.
Response
CacheControl::do_fetch(const Request& request, asio::yield_context yield)
{
    sys::error_code ec;

    if (get(request, http::field::if_none_match)) {
        sys::error_code ec1, ec2;

        auto res = do_fetch_fresh(request, yield[ec1]);
        if (!ec1) return res;

        auto cache_entry = do_fetch_stored(request, yield[ec2]);
        if (!ec2) return add_warning( move(cache_entry.response)
                                    , "111 Ouinet \"Revalidation Failed\"");

        return or_throw(yield, ec1, move(res));
    }

    // We could attempt retrieval from cache and then validation against the origin,
    // but for the moment we go straight to the origin (RFC7234#5.2.1.4).
    if (has_cache_control_directive(request, "no-cache")) {
        return do_fetch_fresh(request, yield);
    }

    auto cache_entry = do_fetch_stored(request, yield[ec]);

    if (ec && ec != asio::error::operation_not_supported
           && ec != asio::error::not_found) {
        return or_throw<Response>(yield, ec);
    }

    if (ec) {
        return do_fetch_fresh(request, yield);
    }

    if (has_cache_control_directive(cache_entry.response, "private")
        || is_older_than_max_cache_age(cache_entry.time_stamp)) {
        auto response = do_fetch_fresh(request, yield[ec]);

        if (!ec) return response;

        return is_expired(cache_entry)
             ? add_stale_warning(move(cache_entry.response))
             : cache_entry.response;
    }

    if (!is_expired(cache_entry)) {
        return cache_entry.response;
    }

    auto cache_etag  = get(cache_entry.response, http::field::etag);
    auto rq_etag = get(request, http::field::if_none_match);

    if (cache_etag && !rq_etag) {
        auto rq = request; // Make a copy because `request` is const&.

        rq.set(http::field::if_none_match, *cache_etag);

        auto response = do_fetch_fresh(rq, yield[ec]);

        if (ec) {
            return add_stale_warning(move(cache_entry.response));
        }

        if (response.result() == http::status::found) {
            return move(cache_entry.response);
        }

        return response;
    }

    auto response = do_fetch_fresh(request, yield[ec]);

    return ec
         ? add_stale_warning(move(cache_entry.response))
         : response;
}

void CacheControl::max_cached_age(const posix_time::time_duration& d)
{
    _max_cached_age = d;
}

posix_time::time_duration CacheControl::max_cached_age() const
{
    return _max_cached_age;
}

Response
CacheControl::do_fetch_fresh(const Request& rq, asio::yield_context yield)
{
    if (fetch_fresh) {
        sys::error_code ec;
        auto rs = fetch_fresh(rq, yield[ec]);
        if (!ec) { try_to_cache(rq, rs); }
        return or_throw(yield, ec, move(rs));
    }
    return or_throw<Response>(yield, asio::error::operation_not_supported);
}

CacheControl::CacheEntry
CacheControl::do_fetch_stored(const Request& rq, asio::yield_context yield)
{
    if (fetch_stored) {
        return fetch_stored(rq, yield);
    }
    return or_throw<CacheEntry>(yield, asio::error::operation_not_supported);
}

//------------------------------------------------------------------------------
static bool contains_private_data(const http::request_header<>& request)
{
    for (auto& field : request) {
        if(!util::field_is_one_of(field
                , http::field::host
                , http::field::user_agent
                , http::field::accept
                , http::field::accept_language
                , http::field::accept_encoding
                , http::field::keep_alive
                , http::field::connection
                , http::field::referer
                // https://www.w3.org/TR/upgrade-insecure-requests/
                , "Upgrade-Insecure-Requests"
                // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/DNT
                , "DNT")) {
            return true;
        }
    }

    // TODO: This may be a bit too agressive.
    if (request.method() != http::verb::get) {
        return true;
    }

    if (!split_string_pair(request.target(), '?').second.empty()) {
        return true;
    }

    return false;
}

//------------------------------------------------------------------------------
// Cache control:
// https://tools.ietf.org/html/rfc7234
// https://tools.ietf.org/html/rfc5861
// https://tools.ietf.org/html/rfc8246
//
// For a less dry reading:
// https://developers.google.com/web/fundamentals/performance/optimizing-content-efficiency/http-caching
//
// TODO: This function is incomplete.
static bool ok_to_cache( const http::request_header<>&  request
                       , const http::response_header<>& response)
{
    using boost::iequals;

    switch (response.result()) {
        case http::status::ok:
        case http::status::moved_permanently:
            break;
        // TODO: Other response codes
        default:
            return false;
    }

    auto req_cache_control_i = request.find(http::field::cache_control);

    if (req_cache_control_i != request.end()) {
        for (auto v : SplitString(req_cache_control_i->value(), ',')) {
            // https://tools.ietf.org/html/rfc7234#section-3 (bullet #3)
            if (iequals(v, "no-store")) return false;
        }
    }

    auto res_cache_control_i = response.find(http::field::cache_control);

    // https://tools.ietf.org/html/rfc7234#section-3 (bullet #5)
    if (request.count(http::field::authorization)) {
        // https://tools.ietf.org/html/rfc7234#section-3.2
        if (res_cache_control_i == response.end()) return false;

        bool allowed = false;

        for (auto v : SplitString(res_cache_control_i->value(), ',')) {
            if (iequals(v,"must-revalidate")) { allowed = true; break; }
            if (iequals(v,"public"))          { allowed = true; break; }
            if (iequals(v,"s-maxage"))        { allowed = true; break; }
        }

        if (!allowed) return false;
    }

    if (res_cache_control_i == response.end()) return true;

    for (auto kv : SplitString(res_cache_control_i->value(), ','))
    {
        beast::string_view key, val;
        std::tie(key, val) = split_string_pair(kv, '=');

        // https://tools.ietf.org/html/rfc7234#section-3 (bullet #3)
        if (iequals(key, "no-store")) return false;
        // https://tools.ietf.org/html/rfc7234#section-3 (bullet #4)
        if (iequals(key, "private"))  {
            // NOTE: This decision based on the request having private data is
            // our extension (NOT part of RFC). Some servers (e.g.
            // www.bbc.com/) sometimes respond with 'Cache-Control: private'
            // even though the request doesn't contain any private data (e.g.
            // Cookies, {GET,POST,...} variables,...).  We believe this happens
            // when the server serves different content depending on the
            // client's geo location. While we don't necessarily want to break
            // this intent, we believe serving _some_ content is better than
            // none. As such, the client should always check for presence of
            // this 'private' field when fetching from distributed cache and
            // - if present - re-fetch from origin if possible.
            if (contains_private_data(request)) {
                return false;
            }
        }
    }

    return true;
}

//------------------------------------------------------------------------------
void
CacheControl::try_to_cache( const Request& request
                          , const Response& response) const
{
    if (!store) return;
    if (!ok_to_cache(request, response)) return;

    // TODO: This list was created by going through some 100 responses from
    // bbc.com. Careful selection from all possible (standard) fields is
    // needed.
    auto filtered_response =
        util::filter_fields( response
                           , http::field::server
                           , http::field::retry_after
                           , http::field::content_length
                           , http::field::content_type
                           , http::field::content_encoding
                           , http::field::content_language
                           , http::field::accept_ranges
                           , http::field::etag
                           , http::field::age
                           , http::field::date
                           , http::field::expires
                           , http::field::via
                           , http::field::vary
                           , http::field::connection
                           , http::field::location
                           , http::field::cache_control
                           , http::field::warning
                           , http::field::last_modified
                           // # CORS response headers (following <https://fetch.spec.whatwg.org/#http-responses>)
                           , http::field::access_control_allow_origin  // origins the response may be shared with
                           // A request which caused a response with ``Access-Control-Allow-Credentials: true``
                           // probably carried authentication tokens and it should not have been cached anyway,
                           // however a server may erroneously include it for requests not using credentials,
                           // and we do not want to block them.
                           // See <https://stackoverflow.com/a/24689738> for an explanation of the header.
                           , http::field::access_control_allow_credentials  // resp to req w/credentials may be shared
                           // These response headers should only appear in
                           // responses to pre-flight (OPTIONS) requests, which should not be cached.
                           // However, some servers include them as part of responses to GET requests,
                           // so include them since they are not problematic either.
                           , http::field::access_control_allow_methods  // methods allowed in CORS request
                           , http::field::access_control_allow_headers  // headers allowed in CORS request
                           , http::field::access_control_max_age  // expiration of pre-flight response info
                           //
                           , "Access-Control-Expose-Headers"  // headers of response to be exposed
                           );

    // TODO: Apply similar filter to the request.
    store(request, filtered_response);
}

