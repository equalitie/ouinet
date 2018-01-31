#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>

#include "cache_control.h"
#include "or_throw.h"
#include "split_string.h"
#include "util.h"

using namespace std;
using namespace ouinet;

using string_view = beast::string_view;
using Response = CacheControl::Response;
using boost::optional;

namespace posix_time = boost::posix_time;

static
bool has_private_cache_control(const Response& response)
{
    auto cache_control_i = response.find(http::field::cache_control);
    if (cache_control_i == response.end()) return false;

    for (auto kv : SplitString(cache_control_i->value(), ',')) {
        if (boost::iequals(kv, "private")) return true;
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

// TODO: This function is heavily unfinished.
Response
CacheControl::fetch(const Request& request, asio::yield_context yield)
{
    sys::error_code ec;
    auto cache_entry = fetch_from_cache(request, yield[ec]);

    if (ec && ec != asio::error::operation_not_supported
           && ec != asio::error::not_found) {
        return or_throw<Response>(yield, ec);
    }

    if (ec) {
        return fetch_from_origin(request, yield);
    }

    if (has_private_cache_control(cache_entry.response)
        || is_older_than_max_cache_age(cache_entry.time_stamp)) {
        auto response = fetch_from_origin(request, yield[ec]);

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

        auto response = fetch_from_origin(rq, yield[ec]);

        if (ec) {
            return add_stale_warning(move(cache_entry.response));
        }

        if (response.result() == http::status::found) {
            return move(cache_entry.response);
        }

        return response;
    }

    auto response = fetch_from_origin(request, yield[ec]);

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
