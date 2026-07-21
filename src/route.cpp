#include "route.h"
#include "client_config.h"
#include "http_util.h"
#include "util/overloaded.h"

namespace ouinet {

using RequestHdr = http::request_header<>;

static bool match(boost::string_view field, const boost::regex& regex) {
    return boost::regex_match(field.begin(), field.end(), regex);
}

static bool match(boost::string_view field, const std::string_view str) {
    return match(field, boost::regex(str.begin(), str.end()));
}

static Route choose_by_request(const RequestHdr& req, const ClientConfig& config)
{
    auto get_method_override([](const RequestHdr& r) {return r["X-HTTP-Method-Override"];});
    auto get_method([](const RequestHdr& r) {return r.method_string();});
    auto get_host([](const RequestHdr& r) {return r[http::field::host];});
    auto get_hostname([](const RequestHdr& r) {return util::split_ep(r[http::field::host]).first;});
    auto get_x_private([](const RequestHdr& r) {return r[http_::request_private_hdr];});
    auto get_target([](const RequestHdr& r) {return r.target();});

    auto local_rx = util::str("https?://[^:/]+\\.", config.local_domain(), "(:[0-9]+)?/.*");

    // Flags for normal, case-insensitive regular expression.
    static const auto rx_icase = boost::regex::normal | boost::regex::icase;

    static const boost::regex localhost_exact_rx{"localhost", rx_icase};

    // Handle requests to <http://localhost/> internally.
    if (match(get_host(req), localhost_exact_rx)) {
        return Route::FrontEnd();
    }

    if (match(get_host(req), util::str(config.front_end_endpoint()))) {
        return Route::FrontEnd();
    }

    // Other requests to the local host should not use the network
    // to avoid leaking internal services accessed through the client.
    if (!config.is_private_target_allowed()) {
        if (match(get_hostname(req), util::localhost_rx)) {
            return Route::Origin();
        }
    }

    // Access to sites under the local TLD are always accessible
    // with good connectivity, so always use the Origin channel
    // and never cache them.
    if (match(get_target(req), local_rx)) {
        return Route::Origin();
    }

    // Do not use caching for requests tagged as private with Ouinet headers.
    if (match(get_x_private(req), boost::regex(http_::request_private_true, rx_icase))) {
        return Route::OriginOrBlindInjector();
    }

    // When to try to cache or not, depending on the request method:
    //
    //   - Unsafe methods (CONNECT, DELETE, PATCH, POST, PUT): do not cache
    //   - Safe but uncacheable methods (OPTIONS, TRACE): do not cache
    //   - Safe and cacheable (GET, HEAD): cache
    //
    // Thus the only remaining method that implies caching is GET.
    if (!match(get_method(req), "(GET|HEAD)")) {
        return Route::OriginOrBlindInjector();
    }

    // Requests declaring a method override are checked by that method.
    // This is not a standard header,
    // but for instance Firefox uses it for Safe Browsing requests,
    // which according to this standard should actually be POST requests
    // (probably in the hopes of having more chances that requests get through,
    // in spite of using HTTPS).
    if (!match(get_method_override(req), "(|GET)")) {
        return Route::OriginOrBlindInjector();
    }

    // Requests to the private addresses should not use the network
    // to avoid leaking internal services accessed through the client,
    // unless the option `allow-private-targets` is set to true.
    if (!config.is_private_target_allowed()) {
        if (match(get_hostname(req), util::private_addr_rx)) {
            return Route::Origin();
        }
    }

    return Route::OriginOrPublicInjectorOrDCache();
}

static void remove_origin_access(std::optional<Route>& route) {
    if (!route) return;

    return std::visit(overloaded {
            [&] (Route::Origin&) {
                route = std::nullopt;
            },
            [&] (Route::OriginOrDCache&) {
                route = Route::DCache();
            },
            [&] (Route::OriginOrBlindInjector&) {
                route = Route::BlindInjector();
            },
            [&] (Route::OriginOrPublicInjector&) {
                route = Route::PublicInjector();
            },
            [&] (Route::OriginOrPublicInjectorOrDCache&) {
                route = Route::PublicInjectorOrDCache();
            },
            [&] (auto&) {}
        },
        route->value);
}

static void remove_blind_injector_access(std::optional<Route>& route) {
    if (!route) return;

    return std::visit(overloaded {
            [&] (Route::BlindInjector&) {
                route = std::nullopt;
            },
            [&] (Route::OriginOrBlindInjector&) {
                route = Route::Origin();
            },
            [&] (auto&) {}
        },
        route->value);
}

static void remove_public_injector_access(std::optional<Route>& route) {
    if (!route) return;

    return std::visit(overloaded {
            [&] (Route::PublicInjector&) {
                route = std::nullopt;
            },
            [&] (Route::OriginOrPublicInjector&) {
                route = Route::Origin();
            },
            [&] (Route::PublicInjectorOrDCache&) {
                route = Route::DCache();
            },
            [&] (Route::OriginOrPublicInjectorOrDCache&) {
                route = Route::OriginOrDCache();
            },
            [&] (auto&) {}
        },
        route->value);
}

static void remove_dcache_access(std::optional<Route>& route) {
    if (!route) return;

    return std::visit(overloaded {
            [&] (Route::DCache&) {
                route = std::nullopt;
            },
            [&] (Route::OriginOrDCache&) {
                route = Route::Origin();
            },
            [&] (Route::PublicInjectorOrDCache&) {
                route = Route::PublicInjector();
            },
            [&] (Route::OriginOrPublicInjectorOrDCache&) {
                route = Route::OriginOrPublicInjector();
            },
            [&] (auto&) {}
        },
        route->value);
}

std::optional<Route> Route::choose(const RequestHdr& req, const ClientConfig& config) {
    std::optional<Route> route = choose_by_request(req, config);

    if (!config.is_origin_access_enabled()) {
        remove_origin_access(route);
    }

    if (!config.is_proxy_access_enabled()) {
        remove_blind_injector_access(route);
    }

    if (!config.is_injector_access_enabled()) {
        remove_public_injector_access(route);
    }

    if (!config.is_cache_access_enabled()) {
        remove_dcache_access(route);
    }

    return route;
}

#define DEF_OSTREAM(R) \
    std::ostream& operator<<(std::ostream& os, Route::R const&) { return os << #R; }

DEF_OSTREAM(FrontEnd)
DEF_OSTREAM(Origin)
DEF_OSTREAM(BlindInjector)
DEF_OSTREAM(OriginOrBlindInjector)
DEF_OSTREAM(PublicInjector)
DEF_OSTREAM(DCache)
DEF_OSTREAM(OriginOrDCache)
DEF_OSTREAM(OriginOrPublicInjector)
DEF_OSTREAM(PublicInjectorOrDCache)
DEF_OSTREAM(OriginOrPublicInjectorOrDCache)

#undef DEF_OSTREAM

std::ostream& operator<<(std::ostream& os, Route const& route) {
    std::visit([&os] (auto& r) { os << r; }, route.value);
    return os;
}

} // namespace
