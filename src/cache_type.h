#pragma once

#include <variant>
#include <ostream>

namespace ouinet {

struct InjectingCacheType;

struct CacheType {
    struct Bep5Http {};
    struct Bep3HTTPOverI2P {};
    struct Ouisync {};

    using Alternatives = std::variant<
        Bep5Http,
        Bep3HTTPOverI2P,
        Ouisync
    >;

    Alternatives value;

    CacheType() = delete;
    CacheType(CacheType const&) = default;
    CacheType(CacheType&&) = default;
    CacheType& operator=(CacheType const&) = default;
    CacheType& operator=(CacheType&&) = default;

    CacheType(InjectingCacheType const&);
    CacheType& operator=(InjectingCacheType const&);

    template<class V>
    requires(
        !std::is_same_v<V, CacheType> &&
        std::constructible_from<Alternatives, V>
    )
    CacheType(V&& v) : value(std::forward<V>(v)) {}

    template<class Visitor, class Self>
    decltype(auto) visit(this Self&& self, Visitor&& visitor) {
        return std::visit(std::forward<Visitor>(visitor), std::forward<Self>(self).value);
    }

    friend std::ostream& operator<<(std::ostream&, const Bep5Http&);
    friend std::ostream& operator<<(std::ostream&, const Bep3HTTPOverI2P&);
    friend std::ostream& operator<<(std::ostream&, const Ouisync&);
    friend std::ostream& operator<<(std::ostream&, const CacheType&);

    static std::optional<CacheType> from_str(std::string_view);

    // Historically the command line arguments use different string.
    static std::optional<CacheType> from_cmd_arg(std::string_view);
};

struct InjectingCacheType {
    using Alternatives = std::variant<
        CacheType::Bep5Http,
        CacheType::Bep3HTTPOverI2P
    >;

    Alternatives value;

    InjectingCacheType() = delete;
    InjectingCacheType(InjectingCacheType const&) = default;
    InjectingCacheType(InjectingCacheType&&) = default;
    InjectingCacheType& operator=(InjectingCacheType const&) = default;
    InjectingCacheType& operator=(InjectingCacheType&&) = default;

    template<class V>
    requires(
        !std::is_same_v<V, InjectingCacheType> &&
        std::constructible_from<Alternatives, V>
    )
    InjectingCacheType(V&& v) : value(std::forward<V>(v)) {}

    template<class Alt> bool is() const {
        return std::holds_alternative<Alt>(value);
    }

    static std::optional<InjectingCacheType> from(CacheType);

    static std::optional<InjectingCacheType> from_str(std::string_view);

    friend std::ostream& operator<<(std::ostream&, const InjectingCacheType&);
};

} // namespace
