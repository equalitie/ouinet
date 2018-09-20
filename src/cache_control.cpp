#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>

#include "cache_control.h"
#include "or_throw.h"
#include "split_string.h"
#include "util.h"
#include "http_util.h"

#include "logger.h"

using namespace std;
using namespace ouinet;

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
                                , const beast::string_view& directive)
{
    auto cache_control_i = request.find(http::field::cache_control);
    if (cache_control_i == request.end()) return false;

    for (auto kv : SplitString(cache_control_i->value(), ',')) {
        if (boost::iequals(kv, directive)) return true;
    }

    return false;
}

template<class R>
static optional<beast::string_view> get(const R& r, http::field f)
{
    auto i = r.find(f);
    if (i == r.end())
      return boost::none;
        
    return i->value();
}

inline void trim_quotes(beast::string_view& v) {
    while (v.starts_with('"')) v.remove_prefix(1);
    while (v.ends_with  ('"')) v.remove_suffix(1);
};

posix_time::ptime CacheControl::parse_date(beast::string_view s)
{
    // Trim quotes from the beginning
    while (s.starts_with('"')) s.remove_prefix(1);

    namespace bt = boost::posix_time;

    static const auto format = [](const char* fmt) {
        using std::locale;
        return locale(locale::classic(), new bt::time_input_facet(fmt));
    };

    // https://www.w3.org/Protocols/rfc2616/rfc2616-sec3.html#sec3.3

    // Format spec:
    // http://www.boost.org/doc/libs/1_60_0/doc/html/date_time/date_time_io.html
    static const std::locale formats[] = {
        format("%a, %d %b %Y %H:%M:%S"),
        format("%A, %d-%b-%y %H:%M:%S"),
        // TODO: ANSI C's format not done yet because Boost doesn't seem
        // to support parsing days of month in 1-digit format.
    };

    const size_t formats_n = sizeof(formats)/sizeof(formats[0]);

    bt::ptime pt;

    // https://stackoverflow.com/a/13059195/273348
    struct membuf: std::streambuf {
        membuf(char const* base, size_t size) {
            char* p(const_cast<char*>(base));
            this->setg(p, p, p + size);
        }
    };

    struct imemstream: virtual membuf, std::istream {
        imemstream(beast::string_view s)
            : membuf(s.data(), s.size())
            , std::istream(static_cast<std::streambuf*>(this)) {
        }
    };

    for(size_t i=0; i<formats_n; ++i) {
        imemstream is(s);
        is.istream::imbue(formats[i]);
        is >> pt;
        if(pt != bt::ptime()) return pt;
    }

    return pt;
}

static
optional<unsigned> get_max_age(const beast::string_view& cache_control_value)
{
    using boost::optional;

    optional<unsigned> max_age;
    optional<unsigned> s_maxage;

    auto update_max_age = [] ( optional<unsigned>& max_age
                             , beast::string_view value) {
        trim_quotes(value);

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

        // FIXME: Only if the cache is shared.
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
    // RFC2616: https://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html#sec14.9.3
    static const auto now = [] {
        return posix_time::second_clock::universal_time();
    };

    static const auto http10_is_expired = [](const auto& entry) {
        auto expires = get(entry.response, http::field::expires);

        if (expires) {
            auto exp_date = CacheControl::parse_date(*expires);
            if (exp_date != posix_time::ptime()) {
                return exp_date < now();
            }
        }

        return true;
    };

    auto cache_control_value = get(entry.response, http::field::cache_control);

    if (!cache_control_value) {
        return http10_is_expired(entry);
    }

    optional<unsigned> max_age = get_max_age(*cache_control_value);
    if (!max_age) return http10_is_expired(entry);

    return now() > entry.time_stamp + posix_time::seconds(*max_age);
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

Response CacheControl::bad_gateway(const Request& req, beast::string_view reason)
{
    Response res{http::status::bad_gateway, req.version()};
    res.set(http::field::server, _server_name);
    res.set(http_header_prefix + "Debug", reason);
    res.keep_alive(req.keep_alive());
    res.prepare_payload();
    return res;
}

Response
CacheControl::fetch(const Request& request, Yield yield)
{
    sys::error_code ec;
    auto response = do_fetch(request, yield[ec]);

    if(!ec && !has_correct_content_length(response)) {
#ifndef NDEBUG
        yield.log("::::: CacheControl WARNING Incorrect content length :::::");
        yield.log(request, response);
        yield.log(":::::::::::::::::::::::::::::::::::::::::::::::::::::::::");
#endif
    }

    return or_throw(yield, ec, move(response));
}

static bool must_revalidate(const Request& request)
{
    if (get(request, http::field::if_none_match))
        return true;

    auto cache_control = get(request, http::field::cache_control);

    if (cache_control) {
        auto max_age = get_max_age(*cache_control);

        if (max_age && *max_age == 0) {
            return true;
        }

        for (auto kv : SplitString(*cache_control, ',')) {
            if (boost::iequals(kv, "no-cache")) return true;
            if (boost::iequals(kv, "no-store")) return true;
        }
    }

    return false;
}

// TODO: This function is unfinished.
Response
CacheControl::do_fetch(const Request& request, Yield yield)
{
    namespace err = asio::error;

    sys::error_code ec;

    if (must_revalidate(request)) {
        sys::error_code ec1, ec2;

        auto res = do_fetch_fresh(request, yield[ec1]);
        if (!ec1) return res;

        auto cache_entry = do_fetch_stored(request, yield[ec2]);
        if (!ec2) return add_warning( move(cache_entry.response)
                                    , "111 Ouinet \"Revalidation Failed\"");

        if (ec1 == err::operation_aborted || ec2 == err::operation_aborted) {
            return or_throw(yield, err::operation_aborted, move(res));
        }

        return bad_gateway( request
                          , util::str( "1: fresh: \"", ec1.message(), "\""
                                     , " cache: \"",   ec2.message(), "\""));
    }

    auto cache_entry = do_fetch_stored(request, yield[ec]);

    if (ec && ec != err::operation_not_supported
           && ec != err::not_found) {
        return or_throw<Response>(yield, ec);
    }

    if (ec) {
        // Retrieving from cache failed.
        sys::error_code fresh_ec;

        auto res = do_fetch_fresh(request, yield[fresh_ec]);

        if (!fresh_ec) return res;

        if (fresh_ec == err::operation_aborted) {
            return or_throw<Response>(yield, ec);
        }

        return bad_gateway( request
                          , util::str( "2: fresh: \"", fresh_ec.message(), "\""
                                     , " cached: \"", ec.message(), "\""));
    }

    // If we're here that means that we were able to retrieve something
    // from the cache.
    LOG_DEBUG(yield.tag(), ": Response was retrieved from cache");

    if (has_cache_control_directive(cache_entry.response, "private")
        || is_older_than_max_cache_age(cache_entry.time_stamp)) {
        auto response = do_fetch_fresh(request, yield[ec]);

        if (!ec) {
            LOG_DEBUG(yield.tag(), ": Response was served from injector: cached response is private or too old");
            return response;
        }

        LOG_DEBUG(yield.tag(), ": Response was served from cached: cannot reach the injector");

        return is_expired(cache_entry)
             ? add_stale_warning(move(cache_entry.response))
             : cache_entry.response;
    }

    if (!is_expired(cache_entry)) {
        LOG_DEBUG(yield.tag(), ": Response was served from cache: not expired");
        return cache_entry.response;
    }

    auto cache_etag  = get(cache_entry.response, http::field::etag);
    auto rq_etag = get(request, http::field::if_none_match);

    if (cache_etag && !rq_etag) {
        LOG_DEBUG(yield.tag(), ": Attempting to revalidate cached response")
        auto rq = request; // Make a copy because `request` is const&.

        rq.set(http::field::if_none_match, *cache_etag);

        auto response = do_fetch_fresh(rq, yield[ec]);

        if (ec) {
            LOG_DEBUG(yield.tag(), ": Response was served from cache: revalidation failed");
            return add_stale_warning(move(cache_entry.response));
        }

        if (response.result() == http::status::not_modified) {
            LOG_DEBUG(yield.tag(), ": Response was served from cache: not modified");
            return move(cache_entry.response);
        }

        LOG_DEBUG(yield.tag(), ": Response was served from injector: cached response is modified");
        return response;
    }

    auto response = do_fetch_fresh(request, yield[ec]);

    if (ec) {
        LOG_DEBUG(yield.tag(), ": Response was served from cache: requesting fresh response failed");
        return add_stale_warning(move(cache_entry.response));
    } else {
        LOG_DEBUG(yield.tag(), ": Response was served from injector: cached expired without etag");
        return response;
    }
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
CacheControl::do_fetch_fresh(const Request& rq, Yield yield)
{
    if (fetch_fresh) {
        sys::error_code ec;
        auto rs = fetch_fresh(rq, yield[ec].tag("fetch_fresh"));
        if (!ec) { try_to_cache(rq, rs, yield); }
        return or_throw(yield, ec, move(rs));
    }
    return or_throw<Response>(yield, asio::error::operation_not_supported);
}

CacheControl::CacheEntry
CacheControl::do_fetch_stored(const Request& rq, Yield yield)
{
    if (fetch_stored) {
        return fetch_stored(rq, yield.tag("fetch_stored"));
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
                , http::field::cache_control
                , http::field::accept
                , http::field::accept_language
                , http::field::accept_encoding
                , http::field::keep_alive
                , http::field::connection
                , http::field::referer
                , http::field::proxy_connection
                , "X-Requested-With"
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
bool CacheControl::ok_to_cache( const http::request_header<>&  request
                              , const http::response_header<>& response
                              , const char** reason)
{
    using boost::iequals;

    switch (response.result()) {
        case http::status::ok:
        case http::status::moved_permanently:
            break;
        // TODO: Other response codes
        default:
            if (reason) *reason = "Response status";
            return false;
    }

    auto req_cache_control_i = request.find(http::field::cache_control);

    if (req_cache_control_i != request.end()) {
        for (auto v : SplitString(req_cache_control_i->value(), ',')) {
            // https://tools.ietf.org/html/rfc7234#section-3 (bullet #3)
            if (iequals(v, "no-store")) {
                if (reason) *reason = "request has no-store";
                return false;
            }
        }
    }

    auto res_cache_control_i = response.find(http::field::cache_control);

    // https://tools.ietf.org/html/rfc7234#section-3 (bullet #5)
    if (request.count(http::field::authorization)) {
        // https://tools.ietf.org/html/rfc7234#section-3.2
        if (res_cache_control_i == response.end()) {
            if (reason) *reason = "request has auth";
            return false;
        }

        bool allowed = false;

        for (auto v : SplitString(res_cache_control_i->value(), ',')) {
            // FIXME: s-maxage contains '='
            if (iequals(v,"must-revalidate")) { allowed = true; break; }
            if (iequals(v,"public"))          { allowed = true; break; }
            if (iequals(v,"s-maxage"))        { allowed = true; break; }
        }

        if (!allowed) {
            if (reason)
                *reason = "request contains auth, but response's cache control "
                          "header field contains none of "
                          "{must-revalidate, public, s-maxage}";

            return false;
        }
    }

    if (res_cache_control_i == response.end()) return true;

    for (auto kv : SplitString(res_cache_control_i->value(), ','))
    {
        beast::string_view key, val;
        std::tie(key, val) = split_string_pair(kv, '=');

        // https://tools.ietf.org/html/rfc7234#section-3 (bullet #3)
        if (iequals(key, "no-store")) {
            if (reason) *reason = "response contains cache-control: no-store";

            return false;
        }
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
                if (reason)
                    *reason = "response contains cache-control: private";

                return false;
            }
        }
    }

    return true;
}

//------------------------------------------------------------------------------
Response CacheControl::filter_before_store(Response response, bool keep_ouinet)
{
    // TODO: This list was created by going through some 100 responses from
    // bbc.com. Careful selection from all possible (standard) fields is
    // needed.
    return
        util::filter_fields( response
                           , keep_ouinet
                           , http::field::server
                           , http::field::retry_after
                           , http::field::content_length
                           , http::field::content_type
                           , http::field::content_encoding
                           , http::field::content_language
                           , http::field::transfer_encoding
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

}

//------------------------------------------------------------------------------
void
CacheControl::try_to_cache( const Request& request
                          , const Response& response
                          , Yield& yield) const
{
    if (!store) return;

    const char* reason = "";

    if (!ok_to_cache(request, response, &reason)) {
#ifndef NDEBUG
        yield.log("::::: CacheControl: NOT CACHING :::::");
        yield.log(":: ", reason);
        yield.log(request.base(), response.base());
        yield.log(":::::::::::::::::::::::::::::::::::::");
#endif
        return;
    }

    // TODO: Apply similar filter to the request.
    store(request, filter_before_store(response));
}

