#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>

#include "cache_control.h"
#include "or_throw.h"
#include "split_string.h"

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

static
boost::optional<unsigned> get_max_age(const Response& response)
{
    using boost::optional;

    auto cache_control_i = response.find(http::field::cache_control);

    if (cache_control_i == response.end()) return false;

    optional<unsigned> max_age;
    optional<unsigned> s_maxage;

    auto update_max_age = [] (optional<unsigned>& max_age, string_view value) {
        unsigned delta;

        // TODO: What does RFC say about malformed entries?
        try { delta = std::stoi(value.data()); }
        catch (...) { return; }

        if (!max_age || *max_age < delta) {
            max_age = delta;
        }
    };

    for (auto kv : SplitString(cache_control_i->value(), ',')) {
        beast::string_view key, val;
        std::tie(key, val) = split_string_pair(kv, '=');

        if (boost::iequals(key, "s-maxage")) {
            update_max_age(s_maxage, val);
        }

        if (boost::iequals(key, "max_age")) {
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

    optional<unsigned> max_age = get_max_age(entry.response);
    if (!max_age) return false;

    auto now = posix_time::second_clock::universal_time();

    return now <= entry.time_stamp + posix_time::seconds(*max_age);
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
    response[http::field::warning] = value;
    return response;
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
             ? add_warning( move(cache_entry.response)
                          , "110 Ouinet 'Response is stale'")
             : cache_entry.response;
    }

    if (!is_expired(cache_entry)) {
        return cache_entry.response;
    }

    auto response = fetch_from_origin(request, yield[ec]);

    return ec
         ? add_warning( move(cache_entry.response)
                      , "110 Ouinet 'Response is stale'")
         : cache_entry.response;
}

void CacheControl::max_cached_age(const posix_time::time_duration& d)
{
    _max_cached_age = d;
}

posix_time::time_duration CacheControl::max_cached_age() const
{
    return _max_cached_age;
}
