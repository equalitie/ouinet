#pragma once

#include <set>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem.hpp>
#include "../util/executor.h"
#include "../util/signal.h"
#include "../namespaces.h"

namespace ouinet {

using ouinet::util::AsioExecutor;

class BaseDhtGroups {
public:
    using GroupName = std::string;
    using ItemName  = std::string;

public:
    virtual ~BaseDhtGroups() = default;
    virtual std::set<GroupName> groups() const = 0;
    virtual std::set<GroupName> pinned_groups() const = 0;

    // Empty if the group does not exist.
    virtual std::set<ItemName> items(const GroupName&) const = 0;
};

// This is considered read-only and unsafe (so extra checks are performed).
std::unique_ptr<BaseDhtGroups>
load_static_dht_groups(fs::path root_dir, AsioExecutor, Cancel&, asio::yield_context);

class DhtGroups : public BaseDhtGroups {
public:
    virtual ~DhtGroups() = default;

    virtual void add(const GroupName&, const ItemName&, Cancel&, asio::yield_context) = 0;

    // Remove item from every group it is in. Return groups that became empty
    // as a result.
    virtual std::set<GroupName> remove(const ItemName&) = 0;

    // Exclude groups that are explicitly marked as pinned.
    virtual std::set<GroupName> remove(const ItemName&, bool&) = 0;
    virtual bool is_pinned(const GroupName&) = 0;
    virtual void pin_group(const GroupName&) = 0;
    virtual void unpin_group(const GroupName&) = 0;

    // Do nothing if the group does not exist.
    virtual void remove_group(const GroupName&) = 0;
};

// This is considered read-write and safe.
std::unique_ptr<DhtGroups>
load_dht_groups(fs::path root_dir, AsioExecutor, Cancel&, asio::yield_context);

// This is considered read-write and safe.
// When iterating over groups, fallback groups are merged into read-write groups.
// Read-write operations do not affect fallback groups.
// Removal of items does not return groups which remain in fallback groups.
std::unique_ptr<DhtGroups>
load_backed_dht_groups( fs::path root_dir, std::unique_ptr<BaseDhtGroups> fallback_groups
                      , AsioExecutor, Cancel&, asio::yield_context);

} // namespace ouinet
