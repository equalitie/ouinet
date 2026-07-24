#include "route.h"
#include "client_config.h"
#include "http_util.h"
#include "util/overloaded.h"
#include "util/debug.h"

namespace ouinet {

using RequestHdr = http::request_header<>;
using string_view = std::string_view;


static bool match(boost::string_view field, const boost::regex& regex) {
    return boost::regex_match(field.begin(), field.end(), regex);
}

static bool match(boost::string_view field, const std::string_view str) {
    return match(field, boost::regex(str.begin(), str.end()));
}

string_view strip(string_view s) {
    while (s.starts_with(' ')) s.remove_prefix(1);
    while (s.ends_with(' ')) s.remove_suffix(1);
    return s;
}

template<class T> struct TypeTo;

#define FOR_EACH_ROUTE(macro) \
    macro(FrontEnd) \
    macro(Origin) \
    macro(BlindInjector) \
    macro(OriginOrBlindInjector) \
    macro(PublicInjector) \
    macro(DCache) \
    macro(OriginOrDCache) \
    macro(OriginOrPublicInjector) \
    macro(PublicInjectorOrDCache) \
    macro(OriginOrPublicInjectorOrDCache)

#define DEF_TYPE_TO_STR(T) \
    template<> struct TypeTo<Route::T> { static constexpr string_view str() { return #T; } };

FOR_EACH_ROUTE(DEF_TYPE_TO_STR)

#undef DEF_TYPE_TO_STR

inline
bool iequal(string_view s0, string_view s1) {
    return s0.size() == s1.size() &&
           std::ranges::equal(s0, s1,
               [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
               });
}

std::optional<string_view> read_token(string_view& s) {
    while (!s.empty() && s.starts_with(' ')) s.remove_prefix(1);
    size_t size = 0;
    while (size < s.size() && s[size] != ' ') {
        ++size;
    }
    if (size == 0) return {};
    auto ret = s.substr(0, size);
    s.remove_prefix(size);
    return ret;
}

template<class R> struct ParseNoArgs {
    static std::optional<R> parse(string_view& s) {
        if (!strip(s).empty()) return {};
        return R{};
    }
};

template<class C> std::optional<C> read_cache_type(string_view& s) {
    auto ss = s;
    if (strip(ss).empty()) return {};
    auto token = read_token(ss);
    if (!token) return {};
    auto cache_type = C::from_str(*token);
    if (!cache_type) return {};
    s = ss;
    return std::move(*cache_type);
}

template<class R> struct ParseCacheTypeArg {
    static std::optional<R> parse(string_view& s) {
        auto cache_type = read_cache_type<CacheType>(s);
        if (!cache_type) return {};
        if (read_token(s)) return {}; // no other parameters
        return R { std::move(*cache_type) };
    }
};

template<class R> struct ParseInjectingCacheTypeArg {
    static std::optional<R> parse(string_view& s) {
        auto cache_type = read_cache_type<InjectingCacheType>(s);
        if (!cache_type) return {};
        if (read_token(s)) return {}; // no other parameters
        return R { std::move(*cache_type) };
    }
};

template<class R> struct RouteArgs {};

#define DEF_ROUTE_ARGS(R, Parser) \
    template<> struct RouteArgs<Route::R> : Parser<Route::R> {};

DEF_ROUTE_ARGS(FrontEnd,                       ParseNoArgs);
DEF_ROUTE_ARGS(Origin,                         ParseNoArgs);
DEF_ROUTE_ARGS(BlindInjector,                  ParseInjectingCacheTypeArg);
DEF_ROUTE_ARGS(OriginOrBlindInjector,          ParseInjectingCacheTypeArg);
DEF_ROUTE_ARGS(PublicInjector,                 ParseInjectingCacheTypeArg);
DEF_ROUTE_ARGS(DCache,                         ParseCacheTypeArg);
DEF_ROUTE_ARGS(OriginOrDCache,                 ParseCacheTypeArg);
DEF_ROUTE_ARGS(OriginOrPublicInjector,         ParseInjectingCacheTypeArg);
DEF_ROUTE_ARGS(PublicInjectorOrDCache,         ParseInjectingCacheTypeArg);
DEF_ROUTE_ARGS(OriginOrPublicInjectorOrDCache, ParseInjectingCacheTypeArg);

#undef DEF_ROUTE_ARGS

template<class T>
std::optional<Route> read_route(string_view& s) {
    auto ss = s;
    auto name = read_token(ss);
    if (!name) return {};
    if (!iequal(*name, TypeTo<T>::str())) return {};
    auto r = RouteArgs<T>::parse(ss);
    if (r) s = ss;
    return r;
}

static std::optional<Route> choose_by_route_field(const RequestHdr& rq) {
    auto s = strip(rq["X-Ouinet-Route"]);

    #define IF_PARSED_THEN_RETURN(T) if (auto r = read_route<Route::T>(s)) { return *r; }
    FOR_EACH_ROUTE(IF_PARSED_THEN_RETURN)
    #undef IF_PARSED_THEN_RETURN

    return std::nullopt;
}

static void remove_origin_access(std::optional<Route>& route) {
    if (!route) return;

    return std::visit(overloaded {
            [&] (Route::Origin&) {
                route = std::nullopt;
            },
            [&] (Route::OriginOrDCache& r) {
                route = Route::DCache{ r.cache_type };
            },
            [&] (Route::OriginOrBlindInjector& r) {
                route = Route::BlindInjector{ r.cache_type };
            },
            [&] (Route::OriginOrPublicInjector& r) {
                route = Route::PublicInjector{ r.cache_type };
            },
            [&] (Route::OriginOrPublicInjectorOrDCache& r) {
                route = Route::PublicInjectorOrDCache{ r.cache_type };
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
            [&] (Route::PublicInjectorOrDCache& r) {
                route = Route::DCache { r.cache_type };
            },
            [&] (Route::OriginOrPublicInjectorOrDCache& r) {
                route = Route::OriginOrDCache { r.cache_type };
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
            [&] (Route::PublicInjectorOrDCache& r) {
                route = Route::PublicInjector{ r.cache_type };
            },
            [&] (Route::OriginOrPublicInjectorOrDCache& r) {
                route = Route::OriginOrPublicInjector{ r.cache_type };
            },
            [&] (auto&) {}
        },
        route->value);
}

static void public_to_private(std::optional<Route>& route) {
    if (!route) return;

    return std::visit(overloaded {
            [&] (Route::PublicInjector& r) {
                route = Route::BlindInjector{ r.cache_type };
            },
            [&] (Route::DCache&) {
                route = std::nullopt;
            },
            [&] (Route::OriginOrDCache&) {
                route = Route::Origin();
            },
            [&] (Route::OriginOrPublicInjector& r) {
                route = Route::OriginOrBlindInjector{ r.cache_type };
            },
            [&] (Route::PublicInjectorOrDCache& r) {
                route = Route::BlindInjector{ r.cache_type };
            },
            [&] (Route::OriginOrPublicInjectorOrDCache& r) {
                route = Route::OriginOrBlindInjector{ r.cache_type };
            },
            [&] (auto&) {}
        },
        route->value);
}

static std::optional<CacheType> get_cache_type(std::optional<Route> const& route) {
    if (!route) return {};

    using R = std::optional<CacheType>;

    return std::visit(overloaded {
            [] (const Route::FrontEnd& r)                       -> R { return {}; },
            [] (const Route::Origin& r)                         -> R { return {}; },
            [] (const Route::BlindInjector& r)                  -> R { return r.cache_type; },
            [] (const Route::OriginOrBlindInjector& r)          -> R { return r.cache_type; },
            [] (const Route::PublicInjector& r)                 -> R { return r.cache_type; },
            [] (const Route::DCache& r)                         -> R { return r.cache_type; },
            [] (const Route::OriginOrDCache& r)                 -> R { return r.cache_type; },
            [] (const Route::OriginOrPublicInjector& r)         -> R { return r.cache_type; },
            [] (const Route::PublicInjectorOrDCache& r)         -> R { return r.cache_type; },
            [] (const Route::OriginOrPublicInjectorOrDCache& r) -> R { return r.cache_type; },
        },
        route->value);
}

static void apply_safety_restrictions(std::optional<Route>& route, const RequestHdr& req, const ClientConfig& config)
{
    if (!route) return;

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
        route = Route::FrontEnd();
        return;
    }

    if (match(get_host(req), util::str(config.front_end_endpoint()))) {
        route = Route::FrontEnd();
        return;
    }

    // Other requests to the local host should not use the network
    // to avoid leaking internal services accessed through the client.
    if (!config.is_private_target_allowed()) {
        if (match(get_hostname(req), util::localhost_rx)) {
            route = Route::Origin();
            return;
        }
    }

    // Access to sites under the local TLD are always accessible
    // with good connectivity, so always use the Origin channel
    // and never cache them.
    if (match(get_target(req), local_rx)) {
        route = Route::Origin();
        return;
    }

    // Do not use caching for requests tagged as private with Ouinet headers.
    if (match(get_x_private(req), boost::regex(http_::request_private_true, rx_icase))) {
        return public_to_private(route);
    }

    // When to try to cache or not, depending on the request method:
    //
    //   - Unsafe methods (CONNECT, DELETE, PATCH, POST, PUT): do not cache
    //   - Safe but uncacheable methods (OPTIONS, TRACE): do not cache
    //   - Safe and cacheable (GET, HEAD): cache
    //
    // Thus the only remaining method that implies caching is GET.
    if (!match(get_method(req), "(GET|HEAD)")) {
        return public_to_private(route);
    }

    // Requests declaring a method override are checked by that method.
    // This is not a standard header,
    // but for instance Firefox uses it for Safe Browsing requests,
    // which according to this standard should actually be POST requests
    // (probably in the hopes of having more chances that requests get through,
    // in spite of using HTTPS).
    if (!match(get_method_override(req), "(|GET)")) {
        return public_to_private(route);
    }

    // Requests to the private addresses should not use the network
    // to avoid leaking internal services accessed through the client,
    // unless the option `allow-private-targets` is set to true.
    if (!config.is_private_target_allowed()) {
        if (match(get_hostname(req), util::private_addr_rx)) {
            route = Route::Origin();
            return;
        }
    }
}

std::optional<Route> apply_config_restrictions(std::optional<Route>& route, const ClientConfig& config) {
    if (!config.is_origin_access_enabled()) {
        remove_origin_access(route);
    }

    if (!config.is_proxy_access_enabled()) {
        remove_blind_injector_access(route);
    }

    if (!config.is_injector_access_enabled()) {
        remove_public_injector_access(route);
    }

    if (auto cache_type = get_cache_type(route)) {
        if (!config.is_cache_enabled(*cache_type)) {
            remove_dcache_access(route);
        }
    }

    return route;
}

std::optional<Route> Route::choose(const RequestHdr& req, const ClientConfig& config) {
    std::optional<Route> route = choose_by_route_field(req);

    // Set to default if no `X-Ouinet-Route` is present.
    if (!route) route = Route::OriginOrPublicInjectorOrDCache{ CacheType::Bep5Http{} };

    apply_safety_restrictions(route, req, config);

    apply_config_restrictions(route, config);

    return route;
}

std::ostream& operator<<(std::ostream& os, Route::FrontEnd const& r) {
    return os << TypeTo<std::decay_t<decltype(r)>>::str();
}

std::ostream& operator<<(std::ostream& os, Route::Origin const& r) {
    return os << TypeTo<std::decay_t<decltype(r)>>::str();
}

std::ostream& operator<<(std::ostream& os, Route::BlindInjector const& r) {
    return os << TypeTo<std::decay_t<decltype(r)>>::str() << " " << r.cache_type;
}

std::ostream& operator<<(std::ostream& os, Route::OriginOrBlindInjector const& r) {
    return os << TypeTo<std::decay_t<decltype(r)>>::str() << " " << r.cache_type;
}

std::ostream& operator<<(std::ostream& os, Route::PublicInjector const& r) {
    return os << TypeTo<std::decay_t<decltype(r)>>::str() << " " << r.cache_type;
};

std::ostream& operator<<(std::ostream& os, Route::DCache const& r) {
    return os << TypeTo<std::decay_t<decltype(r)>>::str() << " " << r.cache_type;
};

std::ostream& operator<<(std::ostream& os, Route::OriginOrDCache const& r) {
    return os << TypeTo<std::decay_t<decltype(r)>>::str() << " " << r.cache_type;
};

std::ostream& operator<<(std::ostream& os, Route::OriginOrPublicInjector const& r) {
    return os << TypeTo<std::decay_t<decltype(r)>>::str() << " " << r.cache_type;
};

std::ostream& operator<<(std::ostream& os, Route::PublicInjectorOrDCache const& r) {
    return os << TypeTo<std::decay_t<decltype(r)>>::str() << " " << r.cache_type;
};

std::ostream& operator<<(std::ostream& os, Route::OriginOrPublicInjectorOrDCache const& r) {
    return os << TypeTo<std::decay_t<decltype(r)>>::str() << " " << r.cache_type;
};

std::ostream& operator<<(std::ostream& os, Route const& route) {
    std::visit([&os] (auto& r) { os << r; }, route.value);
    return os;
}

} // namespace
