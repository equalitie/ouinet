#pragma once

#include "namespaces.h"
#include "cache_type.h"

#include <boost/beast/http/message.hpp>

#include <ostream>
#include <variant>

namespace ouinet {

class ClientConfig;

struct Route {
    struct FrontEnd                       {};
    struct Origin                         {};
    struct BlindInjector                  { InjectingCacheType cache_type; };
    struct OriginOrBlindInjector          { InjectingCacheType cache_type; };
    struct PublicInjector                 { InjectingCacheType cache_type; };
    struct DCache                         { CacheType          cache_type; };
    struct OriginOrDCache                 { CacheType          cache_type; };
    struct OriginOrPublicInjector         { InjectingCacheType cache_type; };
    struct PublicInjectorOrDCache         { InjectingCacheType cache_type; };
    struct OriginOrPublicInjectorOrDCache { InjectingCacheType cache_type; };

    using Alternatives = std::variant<
        FrontEnd,
        Origin,
        BlindInjector,
        OriginOrBlindInjector,
        PublicInjector,
        DCache,
        OriginOrDCache,
        OriginOrPublicInjector,
        PublicInjectorOrDCache,
        OriginOrPublicInjectorOrDCache
    >;

    Route(Route const&) = default;
    Route(Route &&) = default;

    Route& operator=(Route const&) = default;
    Route& operator=(Route&&) = default;

    template<class V>
    requires(
        !std::is_same_v<V, Route> &&
        std::constructible_from<Alternatives, V>
    )
    Route(V&& v) : value(std::forward<V>(v)) {}

    Alternatives value;

    static std::optional<Route> choose(const http::request_header<>&, const ClientConfig&);

    friend std::ostream& operator<<(std::ostream&, FrontEnd const&);
    friend std::ostream& operator<<(std::ostream&, Origin const&);
    friend std::ostream& operator<<(std::ostream&, BlindInjector const&);
    friend std::ostream& operator<<(std::ostream&, OriginOrBlindInjector const&);
    friend std::ostream& operator<<(std::ostream&, PublicInjector const&);
    friend std::ostream& operator<<(std::ostream&, DCache const&);
    friend std::ostream& operator<<(std::ostream&, OriginOrDCache const&);
    friend std::ostream& operator<<(std::ostream&, PublicInjectorOrDCache const&);
    friend std::ostream& operator<<(std::ostream&, OriginOrPublicInjectorOrDCache const&);
    friend std::ostream& operator<<(std::ostream&, Route const&);
};

} // namespace
