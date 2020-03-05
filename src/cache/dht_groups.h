#pragma once

#include <map>
#include <set>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem.hpp>
#include "../util/signal.h"
#include "../namespaces.h"

namespace ouinet {

class DhtGroups {
public:
    using GroupName = std::string;
    using ItemName  = std::string;

    void add(const GroupName&, const ItemName&, Cancel&, asio::yield_context);

    // Remove item from every group it is in. Return groups that became empty
    // as a result.
    std::set<GroupName> remove(const ItemName&);

    static std::unique_ptr<DhtGroups> load(fs::path root_dir, asio::executor, Cancel&, asio::yield_context);

    ~DhtGroups();

    std::set<GroupName> groups() const;

private:
    using Group  = std::pair<GroupName, std::set<ItemName>>;
    using Groups = std::map<GroupName, std::set<ItemName>>;

    DhtGroups(asio::executor, fs::path root_dir, Groups);

    DhtGroups(const DhtGroups&) = delete;
    DhtGroups(DhtGroups&&)      = delete;

    static
    Group load_group(const fs::path dir, asio::executor, Cancel&, asio::yield_context);

    fs::path group_path(const GroupName&);
    fs::path items_path(const GroupName&);
    fs::path item_path(const GroupName&, const ItemName&);

private:
    asio::executor _ex;
    fs::path _root_dir;
    Groups _groups;
    Cancel _lifetime_cancel;
};

} // namespace ouinet
