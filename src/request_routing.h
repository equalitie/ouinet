#pragma once

#include <boost/beast/http.hpp>

#include "namespaces.h"
#include "client_config.h"

namespace ouinet::request_route {

// TODO: It may make sense to split private/dynamic/non-cached channels (origin, proxy)
// from public/static/cached channels (cache/injector)
// so that channels of different types cannot be mixed,
// i.e. it makes no sense to attempt a request which was considered private
// over a public channel like cache or injector,
// and similarly sending a public request to the origin
// misses the opportunity to use the cache for it.
enum class fresh_channel {
    // These channels may be configured by the user.
    origin,             // send request to the origin HTTP server as is
    proxy,              // send request to proxy ouiservice
    injector_or_dcache, // send request to injector ouiservice
    _front_end,         // handle the request internally
};

inline
std::ostream& operator<<(std::ostream& os, fresh_channel ch) {
    switch (ch) {
        case fresh_channel::origin:             os << "origin";             break;
        case fresh_channel::proxy:              os << "proxy";              break;
        case fresh_channel::injector_or_dcache: os << "injector_or_dcache"; break;
        case fresh_channel::_front_end:         os << "_front_end";         break;
        default:                                os << "???";                break;
    }
    return os;
}

// A request router configuration will be
// chosen by the client when receiving a request and
// considered when serving calls from the cache control to
// fetch fresh or cached content, or to cache it.
struct Config {
    // When the cache control decides that a fresh response is needed,
    // attempt those channels in order until one succeeds.
    // If it was the Injector channel, the response may get cached.
    std::deque<fresh_channel> fresh_channels;

    friend
    std::ostream& operator<<(std::ostream&, const ouinet::request_route::Config&);
};

// Route the provided request according to the list of channels associated
// with the first matching expression in the given list,
// otherwise route it according to the given list of default channels.
Config route_choose_config(const http::request_header<>&, const ClientConfig&);

} // namespace ouinet::request_route
