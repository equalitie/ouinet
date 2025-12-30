#pragma once

#include <string>
#include <optional>

namespace ouinet::cache {

// ResourceId identifies HTTP resource, it's derived from the resource URL. It
// is sent to peers to request particular resource. The idea is that while the
// URL is or might be a secret, the ResourceId is not. Thus unless the peer has
// the corresponding resource, they should not be able to obtain back the URL
// out of it without brute force or guessing.
class ResourceId {
public:
    static ResourceId from_url(std::string_view url);

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
    explicit ResourceId(std::string repr);

private:
    std::string _repr;
};

} // namespace ouinet::cache

namespace std {
    std::ostream& operator<<(std::ostream&, const ouinet::cache::ResourceId&);
} // namespace std
