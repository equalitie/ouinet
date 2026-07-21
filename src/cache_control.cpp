#include <boost/algorithm/string.hpp>
#include <boost/asio/error.hpp>
#include <boost/optional.hpp>
#include <optional>
#include <type_traits>
#include <variant>

#include "cache_control.h"
#include "generic_stream.h"
#include "logger.h"
#include "split_string.h"
#include "http_util.h"
#include "parse/number.h"
#include "util.h"
#include "util/condition_variable.h"

namespace ouinet {

using namespace std;

namespace posix_time = boost::posix_time;

// Look for a literal directive (like "no-cache" but not "max-age=N")
// in the "Cache-Control" header field
// of a request or response.
static
bool has_cache_control_directive( const Session& session
                                , const beast::string_view& directive)
{
    auto& hdr = session.response_header();

    auto cache_control_i = hdr.find(http::field::cache_control);
    if (cache_control_i == hdr.end()) return false;

    for (auto kv : SplitString(cache_control_i->value(), ',')) {
        if (boost::iequals(kv, directive)) return true;
    }

    return false;
}

template<class H>
static
const http::fields& fields_of(const H& hdr) {
    return hdr;
}

static
const http::fields& fields_of(const CacheRequest& rq) {
    return rq.header();
}

template<class R>
static boost::optional<beast::string_view> get(const R& r, http::field f)
{
    auto i = fields_of(r).find(f);
    if (i == fields_of(r).end())
      return boost::none;

    return i->value();
}

static boost::optional<beast::string_view> get(const Session& s, http::field f)
{
    auto& hdr = s.response_header();
    return get(hdr, f);
}

inline void trim_quotes(beast::string_view& v) {
    while (v.starts_with('"')) v.remove_prefix(1);
    while (v.ends_with  ('"')) v.remove_suffix(1);
};

static
boost::optional<unsigned> get_max_age(const beast::string_view& cache_control_value)
{
    boost::optional<unsigned> max_age;
    boost::optional<unsigned> s_maxage;

    auto update_max_age = [] ( boost::optional<unsigned>& max_age
                             , beast::string_view value) {
        trim_quotes(value);

        auto opt_delta = parse::number<unsigned>(value);

        // TODO: What does RFC say about malformed entries?
        if (!opt_delta) return;

        if (!max_age || *max_age < *opt_delta) {
            max_age = *opt_delta;
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

/* static */
bool CacheControl::is_expired(const CacheEntry& entry)
{
    auto& hdr = entry.response.response_header();
    return is_expired(hdr, entry.time_stamp);
}

/* static */
bool CacheControl::is_expired( const http::response_header<>& response
                             , boost::posix_time::ptime time_stamp)
{
    // RFC2616: https://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html#sec14.9.3
    static const auto now = [] {
        return posix_time::second_clock::universal_time();
    };

    static const auto http10_is_expired = [](const auto& response) {
        auto expires = get(response, http::field::expires);

        if (expires) {
            auto exp_date = util::parse_date(*expires);
            if (exp_date != posix_time::ptime()) {
                return exp_date < now();
            }
        }

        return true;
    };

    auto cache_control_value = get(response, http::field::cache_control);

    if (!cache_control_value) {
        return http10_is_expired(response);
    }

    boost::optional<unsigned> max_age = get_max_age(*cache_control_value);
    if (!max_age) return http10_is_expired(response);

    return now() > time_stamp + posix_time::seconds(*max_age);
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
Session add_warning(Session s, const char* value)
{
    auto& hdr = s.response_header();
    // Do not use `hdr.set` as several warnings may co-exist
    // (RFC7234#5.5).
    hdr.insert(http::field::warning, value);
    return s;
}

static
Session add_stale_warning(Session response)
{
    return add_warning( std::move(response)
                      , "110 Ouinet \"Response is stale\"");
}

static bool must_revalidate(const CacheRequest& request)
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

std::expected<Session, sys::error_code>
CacheControl::fetch(const CacheRequest& request, Async yield) {
    namespace err = asio::error;

    if (must_revalidate(request)) {
        auto ryield = yield.tag("force_reval");
        LOG_DEBUG(ryield, " User requested revalidation, attempting to fetch fresh");

        auto fresh_result = do_fetch_fresh(request, ryield);
        if (fresh_result) {
            LOG_DEBUG(ryield, " Got revalidated fresh response");
            return std::move(*fresh_result);
        }

        if (fresh_result.error() == err::operation_aborted) {
            LOG_DEBUG(ryield, " Revalidation aborted");
            return std::unexpected(fresh_result.error());
        }

        LOG_DEBUG(ryield, " Revalidation failed, attempting to fetch from cache");
        auto stored_result = do_fetch_stored(request, ryield);
        if (stored_result) {
            LOG_DEBUG(ryield, " Revalidation failed, cached response is stale");
            return add_warning(
                std::move(stored_result->response),
                "111 Ouinet \"Revalidation Failed\""
            );
        }

        LOG_DEBUG(ryield, " Revalidation and cache retrieval failed");
        return std::unexpected(stored_result.error());
    }

    // Fetching from the distributed cache is often very slow and thus we need
    // to fetch from the origin im parallel and then return the first we get.
    std::optional<std::expected<Session, sys::error_code>> fresh_result;
    std::optional<std::expected<CacheEntry, sys::error_code>> stored_result;
    ConditionVariable cv(yield.get_executor());

    // Cancel the child coroutines on scope exit
    Cancel cancel;
    auto cancelled = defer([&] {
        cancel();
    });

    yield.spawn(cancel, [&](auto yield) {
        fresh_result = do_fetch_fresh(request, yield);
        cv.notify();
    });

    yield.spawn(cancel, [&](auto yield) {
       stored_result = do_fetch_stored(request, yield);
       cv.notify();
    });

    // Wait until either one of the job completes successfully or both fail. If one completes
    // successfully, keep the other one running as it might still be needed later.
    while (true) {
        if (fresh_result && *fresh_result) break;
        if (stored_result && *stored_result) break;
        if (fresh_result && stored_result) break;

        cv.wait(yield);
    }

    // Both failed
    if (fresh_result && !*fresh_result && stored_result && !*stored_result) {
        LOG_DEBUG(yield, " Revalidation not needed, fresh and cache retrieval failed");
        return std::unexpected(fresh_result->error()); // arbitrarily return one of the error
    }

    // `fetch_fresh` completed successfully
    if (fresh_result && *fresh_result) {
        LOG_DEBUG(yield, " Fresh retrieval succeeded first");
        return std::move(**fresh_result);
    }

    // `fetch_stored` completed successfully
    LOG_DEBUG(yield, " Response was retrieved from cache");  // used by integration tests
    auto cache_entry = std::move(**stored_result);

    if (has_cache_control_directive(cache_entry.response, "private")
        || is_older_than_max_cache_age(cache_entry.time_stamp)
        || has_temporary_result(cache_entry.response)) {
        auto oyield = yield.tag("cache_old");
        LOG_DEBUG(oyield, " Cached response is private or too old, attempting to fetch fresh");

        // `fetch_fresh` has already been started. Wait for it to complete.
        while (!fresh_result) {
            cv.wait(yield);
        }
        if (*fresh_result) {
            LOG_DEBUG(oyield, " Response was served from injector: cached response is private or too old");
            return std::move(**fresh_result);
        }

        LOG_DEBUG(oyield, " Response was served from cache: cannot reach the injector");

        if (is_expired(cache_entry)) {
            cache_entry.response = add_stale_warning(std::move(cache_entry.response));
        }

        return std::move(cache_entry.response);
    }

    if (!is_expired(cache_entry)) {
        LOG_DEBUG(yield, " Response was served from cache: not expired");
        // yield.cancel();
        return std::move(cache_entry.response);
    }

    auto cache_etag  = get(cache_entry.response, http::field::etag);
    auto rq_etag = get(request, http::field::if_none_match);

    if (cache_etag && !rq_etag) {
        auto ryield = yield.tag("cache_reval");
        LOG_DEBUG(ryield, " Attempting to revalidate cached response");

        auto rq = request;
        rq.set_if_none_match(*cache_etag);

        // Restart `fetch_fresh` with modified request
        cancel();
        auto new_fresh_result = do_fetch_fresh(rq, yield);
        if (!new_fresh_result) {
            LOG_DEBUG(ryield, " Response was served from cache: revalidation failed");
            return add_stale_warning(std::move(cache_entry.response));
        }

        auto response = std::move(*new_fresh_result);
        auto& hdr = response.response_header();

        if (hdr.result() == http::status::not_modified) {
            LOG_DEBUG(ryield, " Response was served from cache: not modified");
            return std::move(cache_entry.response);
        }

        LOG_DEBUG(ryield, " Response was served from injector: cached response is modified");
        return std::move(response);
    }

    {
        auto eyield = yield.tag("cache_notag");
        LOG_DEBUG(eyield, " Cached response has no tag, attempting to fetch fresh");

        // `fetch_fresh` has already been started. Wait for it to complete.
        while (!fresh_result) {
            cv.wait(yield);
        }
        if (!*fresh_result) {
            LOG_DEBUG(eyield, " Response was served from cache: requesting fresh response failed");
            return add_stale_warning(std::move(cache_entry.response));
        }

        LOG_DEBUG(eyield, " Response was served from injector: cached expired without etag");
        return std::move(**fresh_result);
    }
}

//------------------------------------------------------------------------------
bool CacheControl::has_temporary_result(const Session& rs) const
{
    auto& hdr = rs.response_header();

    // TODO: More statuses
    return hdr.result() == http::status::found
        || hdr.result() == http::status::temporary_redirect;
}

//------------------------------------------------------------------------------
void CacheControl::max_cached_age(const posix_time::time_duration& d)
{
    _max_cached_age = d;
}

//------------------------------------------------------------------------------
posix_time::time_duration CacheControl::max_cached_age() const
{
    return _max_cached_age;
}

//------------------------------------------------------------------------------
std::expected<Session, sys::error_code>
CacheControl::do_fetch_fresh(const CacheRequest& rq, Async yield) {
    if (!fetch_fresh) {
        LOG_DEBUG(yield, " No fetch fresh operation");
        return std::unexpected(asio::error::operation_not_supported);
    }

    return fetch_fresh(rq.to_inject_request(), yield);
}

std::expected<CacheControl::CacheEntry, sys::error_code>
CacheControl::do_fetch_stored(const CacheRequest& rq, Async yield) {
    if (!fetch_stored) {
        LOG_DEBUG(yield, " No fetch stored_operation provided");
        return std::unexpected(asio::error::operation_not_supported);
    }

    auto session = fetch_stored(rq.to_retrieve_request(), yield);

    if (!session) return std::unexpected(session.error());

    auto tsh = util::http_injection_ts(session->response_header());
    auto ts = parse::number<time_t>(tsh);
    auto date = ts ? boost::posix_time::from_time_t(*ts)
                   : boost::posix_time::not_a_date_time;

    return CacheEntry { date, std::move(*session) };
}

//------------------------------------------------------------------------------
// NOTE: This is *not* used to decide the cacheability of arbitrary responses,
// its is only used as a last resort when
// the origin server already declared the response as private.
static bool contains_private_data(const http::request_header<>& request)
{
    for (auto& field : request) {
        if(!( util::field_is_one_of(field
                , http::field::host
                , http::field::user_agent
                , http::field::cache_control
                , http::field::accept
                , http::field::accept_language
                , http::field::accept_encoding
                , http::field::from
                , http::field::origin
                , http::field::keep_alive
                , http::field::connection
                , http::field::referer
                , http::field::proxy_connection
                , http::field::te
                , "X-Requested-With"
                // https://www.w3.org/TR/upgrade-insecure-requests/
                , "Upgrade-Insecure-Requests"
                // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/DNT
                , "DNT")
             || field.name_string().starts_with(http_::header_prefix))) {
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
                              , bool cache_private
                              , const char** reason)
{
    using boost::iequals;

    switch (response.result()) {
        case http::status::ok:
        case http::status::moved_permanently:
        case http::status::found:
        case http::status::temporary_redirect:
            break;
        // TODO: Other response codes
        default:
            if (reason) *reason = "response status";
            return false;
    }

    auto req_cache_control_i = request.find(http::field::cache_control);

    if (req_cache_control_i != request.end()) {
        for (auto v : SplitString(req_cache_control_i->value(), ',')) {
            // https://tools.ietf.org/html/rfc7234#section-3 (bullet #3)
            if (iequals(v, "no-store")) {
                if (reason) *reason = "request contains \"Cache-Control: no-store\"";
                return false;
            }
        }
    }

    auto res_cache_control_i = response.find(http::field::cache_control);

    // https://tools.ietf.org/html/rfc7234#section-3 (bullet #5)
    if (!cache_private && request.count(http::field::authorization)) {
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
                *reason = "request has auth, but response's \"Cache-Control\""
                          " contains none of {must-revalidate, public, s-maxage}";

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
            if (reason) *reason = "response contains \"Cache-Control: no-store\"";

            return false;
        }
        // https://tools.ietf.org/html/rfc7234#section-3 (bullet #4)
        if (!cache_private && iequals(key, "private"))  {
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
                    *reason = "response contains \"Cache-Control: private\"";

                return false;
            }
        }
    }

    return true;
}

} // namespace ouinet
