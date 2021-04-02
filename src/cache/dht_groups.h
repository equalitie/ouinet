#pragma once

#include <set>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem.hpp>
#include "../util/signal.h"
#include "../namespaces.h"

namespace ouinet {

class BaseDhtGroups {
public:
    using GroupName = std::string;

public:
    virtual ~BaseDhtGroups() = default;
    virtual std::set<GroupName> groups() const = 0;
};

std::unique_ptr<BaseDhtGroups>
load_static_dht_groups(fs::path root_dir, asio::executor, Cancel&, asio::yield_context);

class DhtGroups : public BaseDhtGroups {
public:
    using ItemName  = std::string;

public:
    virtual ~DhtGroups() = default;

    virtual void add(const GroupName&, const ItemName&, Cancel&, asio::yield_context) = 0;

    // Remove item from every group it is in. Return groups that became empty
    // as a result.
    virtual std::set<GroupName> remove(const ItemName&) = 0;
};

std::unique_ptr<DhtGroups>
load_dht_groups(fs::path root_dir, asio::executor, Cancel&, asio::yield_context);

} // namespace ouinet
