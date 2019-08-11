#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>

#include "cache_control.h"
#include "or_throw.h"
#include "split_string.h"
#include "util.h"
#include "http_util.h"
#include "generic_stream.h"
#include "util/async_job.h"
#include "util/condition_variable.h"
#include "util/watch_dog.h"

#include "logger.h"

using namespace std;
using namespace ouinet;

using Request = CacheControl::Request;
using Response = CacheControl::Response;
using Request = CacheControl::Request;

namespace posix_time = boost::posix_time;

// Look for a literal directive (like "no-cache" but not "max-age=N")
// in the "Cache-Control" header field
// of a request or response.
static
bool has_cache_control_directive( const Session& session
                                , const beast::string_view& directive)
{
    auto hdr_p = session.response_header();
    assert(hdr_p);
    if (!hdr_p) return false;

    auto cache_control_i = hdr_p->find(http::field::cache_control);
    if (cache_control_i == hdr_p->end()) return false;

    for (auto kv : SplitString(cache_control_i->value(), ',')) {
        if (boost::iequals(kv, directive)) return true;
    }

    return false;
}

template<class R>
static boost::optional<beast::string_view> get(const R& r, http::field f)
{
    auto i = r.find(f);
    if (i == r.end())
      return boost::none;
        
    return i->value();
}

static boost::optional<beast::string_view> get(const Session& s, http::field f)
{
    auto hdr = s.response_header();
    assert(hdr);
    if (!hdr) return boost::none;
    return get(*hdr, f);
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

/* static */
bool CacheControl::is_expired(const CacheEntry& entry)
{
    auto hdr_p = entry.response.response_header();
    assert(hdr_p);
    if (!hdr_p) return true;
    return is_expired(*hdr_p, entry.time_stamp);
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
    auto hdr_p = s.response_header();

    assert(hdr_p);
    if (!hdr_p) return s;

    hdr_p->set(http::field::warning, value);

    return s;
}

static
Session add_stale_warning(Session response)
{
    return add_warning( move(response)
                      , "110 Ouinet 'Response is stale'");
}

Session
CacheControl::fetch(const Request& request,
                    sys::error_code& fresh_ec,
                    sys::error_code& cache_ec,
                    Cancel& cancel,
                    Yield yield)
{
    sys::error_code ec;

    auto response = do_fetch(
            request,
            fresh_ec,
            cache_ec,
            cancel,
            yield[ec]);

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

//------------------------------------------------------------------------------
bool CacheControl::has_temporary_result(const Session& rs) const
{
    auto hdr = rs.response_header();

    assert(hdr);
    if (!hdr) return false;

    // TODO: More statuses
    return hdr->result() == http::status::found
        || hdr->result() == http::status::temporary_redirect;
}

//------------------------------------------------------------------------------
struct CacheControl::FetchState {
    boost::optional<AsyncJob<Session>> fetch_fresh;
    boost::optional<AsyncJob<CacheEntry>> fetch_stored;
};

//------------------------------------------------------------------------------
Session
CacheControl::do_fetch(
        const Request& request,
        sys::error_code& fresh_ec,
        sys::error_code& cache_ec,
        Cancel& cancel,
        Yield yield)
{
    FetchState fetch_state;

    auto cancel_slot = cancel.connect([&] {
            if (fetch_state.fetch_fresh) fetch_state.fetch_fresh->cancel();
            if (fetch_state.fetch_stored) fetch_state.fetch_stored->cancel();
        });

    auto on_exit = defer([&] {
        auto& fs = fetch_state;
        {
#           ifndef _NDEBUG
            WatchDog wdog(_ios, std::chrono::seconds(10), [&] {
                    yield.log("Fetch fresh failed to stop");
                    assert(0);
                });
#           endif
            if (fs.fetch_fresh)  fs.fetch_fresh ->stop(yield);
        }
        {
#           ifndef _NDEBUG
            WatchDog wdog(_ios, std::chrono::seconds(10), [&] {
                    yield.log("Fetch stored failed to stop");
                    assert(0);
                });
#           endif
            if (fs.fetch_stored) fs.fetch_stored->stop(yield);
        }
    });

    namespace err = asio::error;

    if (must_revalidate(request)) {
        auto res = do_fetch_fresh(fetch_state, request, yield[fresh_ec]);

        if (!fresh_ec) {
            cache_ec = err::operation_aborted;
            return res;
        }

        if (fresh_ec == err::operation_aborted) {
            cache_ec = err::operation_aborted;
            return or_throw(yield, fresh_ec, move(res));
        }

        bool is_fresh = false;
        auto cache_entry = do_fetch_stored(fetch_state, request, is_fresh, yield[cache_ec]);
        if (!cache_ec) {
            if (is_fresh) {
                return move(cache_entry.response);
            }
            return add_warning( move(cache_entry.response)
                                    , "111 Ouinet \"Revalidation Failed\"");
        }

        if (cache_ec == err::operation_aborted)
            return or_throw(yield, fresh_ec, move(res));

        return or_throw<Session>(yield, err::service_not_found);
    }

    bool is_fresh = false;
    auto cache_entry = do_fetch_stored(fetch_state, request, is_fresh, yield[cache_ec]);

    if (cache_ec == err::operation_aborted) {
        fresh_ec = err::operation_aborted;
        return or_throw<Session>(yield, err::operation_aborted);
    }

    if (cache_ec) {
        // Retrieving from cache failed.
        auto res = do_fetch_fresh(fetch_state, request, yield[fresh_ec]);

        if (!fresh_ec) return res;

        if (fresh_ec == err::operation_aborted) {
            return or_throw<Session>(yield, err::operation_aborted);
        }

        return or_throw<Session>(yield, err::no_data);
    }

    if (is_fresh) {
        cache_ec = err::operation_aborted;
        fresh_ec = {};
        return move(cache_entry.response);
    }

    // If we're here that means that we were able to retrieve something
    // from the cache.
    LOG_DEBUG(yield.tag(), ": Response was retrieved from cache");  // used by integration tests

    if (has_cache_control_directive(cache_entry.response, "private")
        || is_older_than_max_cache_age(cache_entry.time_stamp)
        || has_temporary_result(cache_entry.response)) {
        auto response = do_fetch_fresh(fetch_state, request, yield[fresh_ec]);

        if (!fresh_ec) {
            cache_ec = err::operation_aborted;
            LOG_DEBUG(yield.tag(), ": Response was served from injector: cached response is private or too old");
            return response;
        }

        LOG_DEBUG(yield.tag(), ": Response was served from cached: cannot reach the injector");

        if (is_expired(cache_entry)) {
            cache_entry.response = add_stale_warning(move(cache_entry.response));
        }

        return move(cache_entry.response);
    }

    if (!is_expired(cache_entry)) {
        LOG_DEBUG(yield.tag(), ": Response was served from cache: not expired");
        fresh_ec = err::operation_aborted;
        return move(cache_entry.response);
    }

    auto cache_etag  = get(cache_entry.response, http::field::etag);
    auto rq_etag = get(request, http::field::if_none_match);

    if (cache_etag && !rq_etag) {
        LOG_DEBUG(yield.tag(), ": Attempting to revalidate cached response")
        auto rq = request; // Make a copy because `request` is const&.

        rq.set(http::field::if_none_match, *cache_etag);

        auto response = do_fetch_fresh(fetch_state, rq, yield[fresh_ec]);

        if (fresh_ec) {
            LOG_DEBUG(yield.tag(), ": Response was served from cache: revalidation failed");
            return add_stale_warning(move(cache_entry.response));
        }

        auto hdr = response.response_header();
        assert(hdr);

        if (hdr->result() == http::status::not_modified) {
            LOG_DEBUG(yield.tag(), ": Response was served from cache: not modified");
            return move(cache_entry.response);
        }

        LOG_DEBUG(yield.tag(), ": Response was served from injector: cached response is modified");
        return response;
    }

    auto response = do_fetch_fresh(fetch_state, request, yield[fresh_ec]);

    if (fresh_ec) {
        LOG_DEBUG(yield.tag(), ": Response was served from cache: requesting fresh response failed");
        return add_stale_warning(move(cache_entry.response));
    } else {
        cache_ec = err::operation_aborted;
        LOG_DEBUG(yield.tag(), ": Response was served from injector: cached expired without etag");
        return response;
    }
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
auto CacheControl::make_fetch_fresh_job(const Request& rq, Yield& yield)
{
    AsyncJob<Session> job(_ios);

    job.start([&] (Cancel& cancel, asio::yield_context yield_) mutable {
            auto y = yield.detach(yield_);
            sys::error_code ec;
            auto r = fetch_fresh(rq, cancel, y[ec]);
            assert(!cancel || ec == asio::error::operation_aborted);
            if (ec) return or_throw(y, ec, move(r));
            assert(r.response_header());
            return r;
        });

    return job;
}

//------------------------------------------------------------------------------
Session
CacheControl::do_fetch_fresh(FetchState& fs, const Request& rq, Yield yield)
{
    if (!fetch_fresh) {
        return or_throw<Session>(yield, asio::error::operation_not_supported);
    }

    if (!fs.fetch_fresh) {
        fs.fetch_fresh = make_fetch_fresh_job(rq, yield);
    }

    ConditionVariable cv(_ios);
    fs.fetch_fresh->on_finish([&cv] { cv.notify(); });
    cv.wait(yield);

    auto result = move(fs.fetch_fresh->result());
    auto rs = move(result.retval);

    return or_throw(yield, result.ec, move(rs));
}

CacheEntry
CacheControl::do_fetch_stored(FetchState& fs,
                              const Request& rq,
                              bool& is_fresh,
                              Yield yield)
{
    is_fresh = false;
    if (!fetch_stored) {
        return or_throw<CacheEntry>(yield, asio::error::operation_not_supported);
    }

    // Fetching from the distributed cache is often very slow and thus we need
    // to fetch from the origin im parallel and then return the first we get.
    if (_parallel_fetch_enabled && !fs.fetch_fresh) {
        fs.fetch_fresh = make_fetch_fresh_job(rq, yield);
    }

    if (!fs.fetch_stored) {
        fs.fetch_stored = AsyncJob<CacheEntry>(_ios);
        fs.fetch_stored->start(
                [&] (Cancel& cancel, asio::yield_context yield_) mutable {
                    return fetch_stored(rq, cancel, yield.detach(yield_));
                });
    }

    enum Which { fresh, stored, none };
    Which which = none;
    ConditionVariable cv(_ios);

    if (fs.fetch_fresh) {
        fs.fetch_fresh ->on_finish([&] {
                which = fresh;
                fs.fetch_stored->on_finish(nullptr);
                cv.notify();
            });
    }

    fs.fetch_stored->on_finish([&] {
            which = stored;
            if (fs.fetch_fresh) fs.fetch_fresh->on_finish(nullptr);
            cv.notify();
        });

    cv.wait(yield);

    if (which == fresh) {
        auto& r = fs.fetch_fresh->result();
        if (!r.ec) {
            is_fresh = true;
            return {
                posix_time::second_clock::universal_time(),
                move(r.retval)
            };
        }

        // fetch_fresh errored, wait for the stored version
        ConditionVariable cv(_ios);
        fs.fetch_stored->on_finish([&] { cv.notify(); });
        cv.wait(yield);

        auto& r2 = fs.fetch_stored->result();
        return or_throw(yield, r2.ec, move(r2.retval));
    }
    else if (which == stored) {
        auto& r = fs.fetch_stored->result();
        return or_throw(yield, r.ec, move(r.retval));
    }

    assert(0);
    return CacheEntry();
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
