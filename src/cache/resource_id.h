#pragma once

#include <string>
#include <optional>
#include "util/yield.h"
#include "util/scrypt.h"

namespace ouinet::cache {

class ResourceId {
public:
    static std::optional<ResourceId> from_url(std::string_view url, YieldContext);

    static std::optional<ResourceId> from_hex(std::string_view hex);
    static std::optional<ResourceId> from_hex(std::wstring_view hex);

    ResourceId(ResourceId const&) = default;
    ResourceId(ResourceId &&) = default;
    ResourceId& operator=(ResourceId const&) = default;
    ResourceId& operator=(ResourceId &&) = default;

    const std::string& hex_string() const;

    bool operator<(const ResourceId& other) const {
        return _repr < other._repr;
    }

    bool operator==(const ResourceId& other) const {
        return _repr == other._repr;
    }

private:
    explicit ResourceId(std::string repr) : _repr(std::move(repr)) {}

private:
    std::string _repr;
};

} // namespace ouinet::cache

namespace std {
    std::ostream& operator<<(std::ostream&, const ouinet::cache::ResourceId&);
} // namespace std
