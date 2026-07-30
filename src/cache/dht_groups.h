#pragma once

#include <set>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem.hpp>
#include "resource_id.h"
#include "../util/executor.h"
#include "../namespaces.h"
#include <expected>

namespace ouinet {

class Async;
class Cancel;
using ouinet::util::AsioExecutor;

class BaseDhtGroups {
public:
    using GroupName = std::string;

public:
    virtual ~BaseDhtGroups() = default;
    virtual std::set<GroupName> groups() const = 0;
    virtual std::set<GroupName> pinned_groups() const = 0;

    // Empty if the group does not exist.
    virtual std::set<cache::ResourceId> items(const GroupName&) const = 0;
};

// This is considered read-only and unsafe (so extra checks are performed).
[[nodiscard]]
std::expected<std::unique_ptr<BaseDhtGroups>, sys::error_code>
load_static_dht_groups(fs::path root_dir, Async);

class DhtGroups : public BaseDhtGroups {
public:
    virtual ~DhtGroups() = default;

    [[nodiscard]]
    virtual std::expected<void, sys::error_code>
    add(const GroupName&, const cache::ResourceId&, Async) = 0;

    // Remove item from every group it is in. Return groups that became empty
    // as a result.
    virtual std::set<GroupName> remove(const cache::ResourceId&) = 0;

    // Exclude groups that are explicitly marked as pinned.
    virtual std::set<GroupName> remove(const cache::ResourceId&, bool&) = 0;
    virtual bool is_pinned(const GroupName&, sys::error_code&) = 0;
    // Returns true if the resource is in at least in one group that is pinned.
    virtual bool is_pinned(const cache::ResourceId&) = 0;
    virtual bool pin_group(const GroupName&, sys::error_code&) = 0;
    virtual bool unpin_group(const GroupName&, sys::error_code&) = 0;

    // Do nothing if the group does not exist.
    virtual void remove_group(const GroupName&) = 0;
};

// This is considered read-write and safe.
[[nodiscard]]
std::expected<std::unique_ptr<DhtGroups>, sys::error_code>
load_dht_groups(fs::path root_dir, Async);

// This is considered read-write and safe.
// When iterating over groups, fallback groups are merged into read-write groups.
// Read-write operations do not affect fallback groups.
// Removal of items does not return groups which remain in fallback groups.
[[nodiscard]]
std::expected<std::unique_ptr<DhtGroups>, sys::error_code>
load_backed_dht_groups(fs::path root_dir, std::unique_ptr<BaseDhtGroups> fallback_groups, Async);

} // namespace ouinet
