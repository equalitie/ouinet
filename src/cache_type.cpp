#include "cache_type.h"
#include "util/overloaded.h"
#include <algorithm>

namespace ouinet {

#define FOR_EACH_CACHE_TYPE(macro) \
    macro(Bep5Http) \
    macro(Bep3HTTPOverI2P) \
    macro(Ouisync)

// ----

template<class T> struct TypeToStr;
#define DEF_TYPE_TO_STR(T) \
    template<> struct TypeToStr<CacheType::T> { static constexpr std::string_view str() { return #T; } };

FOR_EACH_CACHE_TYPE(DEF_TYPE_TO_STR);

#undef DEF_TYPE_TO_STR

// ----

#define DEF_OSTREAM(T) \
    std::ostream& operator<<(std::ostream& os, const CacheType::T&) { \
        return os << TypeToStr<CacheType::T>::str();\
    }

FOR_EACH_CACHE_TYPE(DEF_OSTREAM);

#undef DEF_OSTREAM

std::ostream& operator<<(std::ostream& os, const CacheType& type) { \
    return std::visit([&os] (const auto& t) -> std::ostream& { return os << t; }, type.value);
}

std::ostream& operator<<(std::ostream& os, const InjectingCacheType& type) { \
    return std::visit([&os] (const auto& t) -> std::ostream& { return os << t; }, type.value);
}

// ----

inline
bool iequal(std::string_view s0, std::string_view s1) {
    return s0.size() == s1.size() &&
           std::ranges::equal(s0, s1,
               [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
               });
}

#define DEF_RETURN_IF_EQUAL(T) if (iequal(s, #T)) return CacheType::T{};

std::optional<CacheType> CacheType::from_str(std::string_view s) {
    FOR_EACH_CACHE_TYPE(DEF_RETURN_IF_EQUAL);
    return {};
}

#undef DEF_RETURN_IF_EQUAL

std::optional<InjectingCacheType> InjectingCacheType::from_str(std::string_view s) {
    auto cache_type = CacheType::from_str(s);
    if (!cache_type) return {};
    return InjectingCacheType::from(*cache_type);
}

// ----

std::optional<CacheType> CacheType::from_cmd_arg(std::string_view s) {
    if (s == "bep5-http") return CacheType::Bep5Http{};
    if (s == "bep3-http-over-i2p") return CacheType::Bep3HTTPOverI2P{};
    if (s == "ouisync") return CacheType::Ouisync{};
    return {};
}

// ----

std::optional<InjectingCacheType> InjectingCacheType::from(CacheType type) {
    using R = std::optional<InjectingCacheType>;
    return std::visit(overloaded {
            [] (CacheType::Bep5Http type)        -> R { return type; },
            [] (CacheType::Bep3HTTPOverI2P type) -> R { return type; },
            [] (CacheType::Ouisync)              -> R { return {}; },
        },
        type.value);
}

// ----

CacheType::CacheType(InjectingCacheType const& other)
    : value(std::visit([] (auto type) -> CacheType::Alternatives { return type; }, other.value))
{}

CacheType& CacheType::operator=(InjectingCacheType const& other) {
    value = std::visit([] (auto type) -> CacheType::Alternatives { return type; }, other.value);
    return *this;
}

} // namespace
