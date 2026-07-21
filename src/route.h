#pragma once

#include "namespaces.h"

#include <boost/beast/http/message.hpp>

#include <ostream>
#include <variant>

namespace ouinet {

class ClientConfig;

struct Route {
    struct FrontEnd {};
    struct Origin {};
    struct BlindInjector {};
    struct OriginOrBlindInjector {};
    struct PublicInjector {};
    struct DCache {};
    struct OriginOrDCache {};
    struct OriginOrPublicInjector {};
    struct PublicInjectorOrDCache {};
    struct OriginOrPublicInjectorOrDCache {};

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
