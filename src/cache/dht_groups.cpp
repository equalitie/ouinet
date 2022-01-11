#include "dht_groups.h"
#include "../logger.h"
#include "../util/file_io.h"
#include "../util/bytes.h"
#include "../util/hash.h"
#include "../util/quote_error_message.h"

#include <algorithm>
#include <map>

using namespace ouinet;

#define _LOGPFX "DHT Groups: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _WARN(...)  LOG_WARN(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)

using asio::yield_context;
namespace file_io = util::file_io;
using sys::errc::make_error_code;

// https://stackoverflow.com/a/417184/273348
#define MAX_URL_SIZE 2000

class DhtGroupsImpl {
public:
    using GroupName = BaseDhtGroups::GroupName;
    using ItemName  = BaseDhtGroups::ItemName;

public:
    ~DhtGroupsImpl();

    static std::unique_ptr<DhtGroupsImpl>
    load_trusted(fs::path root_dir, asio::executor ex, Cancel& c, asio::yield_context y)
    { return load(std::move(root_dir), true, std::move(ex), c, std::move(y)); }

    static std::unique_ptr<DhtGroupsImpl>
    load_untrusted(fs::path root_dir, asio::executor ex, Cancel& c, asio::yield_context y)
    { return load(std::move(root_dir), false, std::move(ex), c, std::move(y)); }

    std::set<GroupName> groups() const;
    std::set<ItemName> items(const GroupName&) const;

    void add(const GroupName&, const ItemName&, Cancel&, asio::yield_context);

    std::set<GroupName> remove(const ItemName&);

    void remove_group(const GroupName&);

private:
    using Group  = std::pair<GroupName, std::set<ItemName>>;
    using Groups = std::map<GroupName, std::set<ItemName>>;

    DhtGroupsImpl(asio::executor, fs::path root_dir, Groups);

    DhtGroupsImpl(const DhtGroupsImpl&) = delete;
    DhtGroupsImpl(DhtGroupsImpl&&)      = delete;

    static
    std::unique_ptr<DhtGroupsImpl>
    load(fs::path root_dir, bool trusted, asio::executor, Cancel&, asio::yield_context);

    static
    Group load_group( const fs::path dir, bool trusted
                    , asio::executor, Cancel&, asio::yield_context);

    fs::path group_path(const GroupName&);
    fs::path items_path(const GroupName&);
    fs::path item_path(const GroupName&, const ItemName&);

private:
    asio::executor _ex;
    fs::path _root_dir;
    Groups _groups;
    Cancel _lifetime_cancel;
};

DhtGroupsImpl::DhtGroupsImpl(asio::executor ex, fs::path root_dir, Groups groups)
    : _ex(ex)
    , _root_dir(std::move(root_dir))
    , _groups(std::move(groups))
{}

static
void
try_remove(const fs::path& path)
{
    _DEBUG("Removing cached response: ", path);
    sys::error_code ec;
    fs::remove_all(path, ec);
    if (ec) _WARN( "Failed to remove cached response: "
                 , path, "; ec=", ec);
    // The parent directory may be left empty.
}

static std::string read_file(fs::path p, asio::executor ex, Cancel& c, yield_context y)
{
    sys::error_code ec;

    if (!fs::is_regular_file(p)) {
        _ERROR("Not a regular file: ", p);
        return or_throw<std::string>(y, make_error_code(sys::errc::invalid_argument));
    }

    auto f = file_io::open_readonly(ex, p, ec);
    if (ec) return or_throw<std::string>(y, ec);

    size_t size = file_io::file_size(f, ec);
    if (ec) return or_throw<std::string>(y, ec);

    if (size > MAX_URL_SIZE)
        return or_throw<std::string>(y, make_error_code(sys::errc::value_too_large));

    std::string ret(size, '\0');
    file_io::read(f, asio::buffer(ret), c, y[ec]);

    return or_throw(y, ec, std::move(ret));
}

std::string sha1_hex_digest(const std::string& s) {
    return util::bytes::to_hex(util::sha1_digest(s));
}

/* static */
DhtGroupsImpl::Group
DhtGroupsImpl::load_group( const fs::path dir
                         , bool trusted
                         , asio::executor ex
                         , Cancel& cancel
                         , yield_context yield)
{
    assert(fs::is_directory(dir));
    sys::error_code ec;

    std::string group_name = read_file(dir/"group_name", ex, cancel, yield[ec]);
    if (ec) return or_throw<Group>(yield, ec);

    if (!trusted && dir.filename() != sha1_hex_digest(group_name)) {
        _ERROR("Group name does not match its path: ", dir);
        return or_throw<Group>(yield, make_error_code(sys::errc::invalid_argument));
    }

    fs::path items_dir = dir/"items";

    if (!fs::exists(items_dir)) {
        return {std::move(group_name), {}};
    }

    if (!fs::is_directory(items_dir)) {
        _ERROR(items_dir, " is not a directory");
        return or_throw<Group>(yield, make_error_code(sys::errc::not_a_directory));
    }

    Group::second_type items;

    for (auto f : fs::directory_iterator(items_dir)) {
        std::string name = read_file(f, ex, cancel, yield[ec]);

        if (cancel) {
            return or_throw<Group>(yield, asio::error::operation_aborted);
        }

        if (ec) {
            if (trusted) try_remove(f);
            continue;
        }

        if (!trusted && f.path().filename() != sha1_hex_digest(name)) {
            _ERROR("Group item name does not match its path: ", dir);
            continue;
        }

        items.insert(name);
    }

    return {std::move(group_name), std::move(items)};
}

std::set<DhtGroups::GroupName> DhtGroupsImpl::groups() const
{
    std::set<DhtGroups::GroupName> ret;

    for (auto& group : _groups) {
        ret.insert(group.first);
    }

    return ret;
}

std::set<DhtGroups::ItemName> DhtGroupsImpl::items(const GroupName& gn) const
{
    std::set<DhtGroups::ItemName> ret;

    auto gi = _groups.find(gn);
    if (gi == _groups.end()) return ret;

    for (auto& item : gi->second) {
        ret.insert(item);
    }

    return ret;
}

/* static */
std::unique_ptr<DhtGroupsImpl>
DhtGroupsImpl::load( fs::path root_dir
                   , bool trusted
                   , asio::executor ex
                   , Cancel& cancel
                   , yield_context yield)
{
    using Ret = std::unique_ptr<DhtGroupsImpl>;
    namespace err = asio::error;

    Groups groups;

    if (fs::exists(root_dir)) {
        if (!fs::is_directory(root_dir)) {
            _ERROR("Not a directory: '", root_dir, "'");
            return or_throw<Ret>(yield, make_error_code(sys::errc::not_a_directory));
        }
    } else if (trusted) {
        sys::error_code ec;
        fs::create_directories(root_dir, ec);
        if (ec) {
            _ERROR("Failed to create directory: ", root_dir, "; ec=", ec);
            return or_throw<Ret>(yield, ec);
        }
    } else {
        _ERROR("Groups directory does not exist: ", root_dir);
        return or_throw<Ret>(yield, make_error_code(sys::errc::no_such_file_or_directory));
    }

    for (auto f : fs::directory_iterator(root_dir)) {
        sys::error_code ec;

        if (!fs::is_directory(f)) {
            _ERROR("Non directory found in '", root_dir, "': '", f, "'");
            continue;
        }

        auto group = load_group(f, trusted, ex, cancel, yield[ec]);

        if (cancel) return or_throw<Ret>(yield, asio::error::operation_aborted);
        if (ec || group.second.empty()) {
            _WARN("Not loading empty group: ", group.first);
            if (trusted) try_remove(f);
            continue;
        }

        groups.insert(std::move(group));
    }

    return std::unique_ptr<DhtGroupsImpl>
        (new DhtGroupsImpl(ex, std::move(root_dir), std::move(groups)));
}

fs::path
DhtGroupsImpl::group_path(const GroupName& group_name)
{
    return _root_dir / sha1_hex_digest(group_name);
}

fs::path
DhtGroupsImpl::items_path(const GroupName& group_name)
{
    return group_path(group_name) / "items";
}

fs::path
DhtGroupsImpl::item_path(const GroupName& group_name, const ItemName& item_name)
{
    return items_path(group_name) / sha1_hex_digest(item_name);
}

void DhtGroupsImpl::add( const GroupName& group_name
                   , const ItemName& item_name
                   , Cancel& cancel
                   , yield_context yield)
{
    _DEBUG("Adding: ", group_name, " -> ", item_name);
    fs::path group_p = group_path(group_name);

    // Create the storage representation of the item in the group.
    if (fs::exists(group_p)) {
        if (!fs::is_directory(group_p)) {
            return or_throw(yield, make_error_code(sys::errc::not_a_directory));
        }
    } else {
        sys::error_code ec;
        fs::create_directories(group_p, ec);
        if (ec) {
            _ERROR("Failed to create directory for group: ", group_name, "; ec=", ec);
            return or_throw(yield, ec);
        }

        auto group_name_f = file_io::open_or_create(_ex, group_p/"group_name", ec);
        if (ec) {
            _ERROR("Failed to create group name file for group: ", group_name, "; ec=", ec);
            try_remove(group_p);
            return or_throw(yield, ec);
        }

        file_io::write(group_name_f, asio::buffer(group_name), cancel, yield[ec]);

        if (ec) {
            if (!cancel) {
                _ERROR("Failed write group name: ", group_name, "; ec=", ec);
            }
            try_remove(group_p);
            return or_throw(yield, ec);
        }
    }

    auto items_p = items_path(group_name);

    sys::error_code ec;
    if (!fs::is_directory(items_p)) {
        fs::create_directories(items_p, ec);
        if (ec) {
            _ERROR("Failed to create items path: ", items_p, "; ec=", ec);
            try_remove(group_p);
            return or_throw(yield, ec);
        }
    }

    auto item_f = file_io::open_or_create(_ex, item_path(group_name, item_name), ec);

    if (ec) {
        _ERROR("Failed to create group item; ec=", ec);
        if (fs::is_empty(items_p)) try_remove(group_p);
        return or_throw(yield, ec);
    }

    file_io::truncate(item_f, 0, ec);

    if (ec) {
        _ERROR("Failed to truncate group item file; ec=", ec);
        if (fs::is_empty(items_p)) try_remove(group_p);
        return or_throw(yield, ec);
    }

    file_io::write(item_f, asio::buffer(item_name), cancel, yield[ec]);

    if (ec) {
        if (!cancel) {
            _ERROR("Failed write to group item; ec=", ec);
        }
        if (fs::is_empty(items_p)) try_remove(group_p);
        return or_throw(yield, ec);
    }

    // Add the item to the group in memory.
    const auto& group_it = _groups.find(group_name);
    if (group_it == _groups.end()) {
        _groups[group_name] = {item_name};  // new group
        return;
    }
    group_it->second.emplace(item_name);  // add item to existing group
}

std::set<DhtGroups::GroupName> DhtGroupsImpl::remove(const ItemName& item_name)
{
    std::set<GroupName> erased_groups;

    for (auto j = _groups.begin(); j != _groups.end();) {
        auto gi = j; ++j;

        auto& group_name = gi->first;
        auto& items      = gi->second;

        if (items.empty()) {
            // This case shouldn't happen, but let's sanitize it anyway.
            erased_groups.insert(group_name);
            try_remove(group_path(group_name));
            _groups.erase(gi);
            continue;
        }

        auto i = items.find(item_name);
        if (i == items.end()) continue;

        items.erase(i);
        try_remove(item_path(group_name, item_name));

        if (items.empty()) {
            erased_groups.insert(group_name);
            try_remove(group_path(group_name));
            _groups.erase(gi);
        }
    }

    return erased_groups;
}

void DhtGroupsImpl::remove_group(const GroupName& gn)
{
    auto gi = _groups.find(gn);
    if (gi == _groups.end()) return;

    try_remove(group_path(gn));
    _groups.erase(gi);
}

DhtGroupsImpl::~DhtGroupsImpl() {
    _lifetime_cancel();
}

class DhtReadGroups : public BaseDhtGroups {
public:
    DhtReadGroups(std::unique_ptr<DhtGroupsImpl> impl)
        : _impl(std::move(impl))
    {}
    ~DhtReadGroups() override = default;

    std::set<GroupName> groups() const override
    { return _impl->groups(); }

    std::set<ItemName> items(const GroupName& gn) const override
    { return _impl->items(gn); }

private:
    std::unique_ptr<DhtGroupsImpl> _impl;
};

std::unique_ptr<BaseDhtGroups>
ouinet::load_static_dht_groups( fs::path root_dir
                              , asio::executor ex
                              , Cancel& cancel
                              , asio::yield_context yield)
{
    // TODO: security checks on loaded files
    return std::make_unique<DhtReadGroups>
        (DhtGroupsImpl::load_untrusted( std::move(root_dir), std::move(ex)
                                      , cancel, std::move(yield)));
}

class FullDhtGroups : public DhtGroups {
public:
    FullDhtGroups(std::unique_ptr<DhtGroupsImpl> impl)
        : _impl(std::move(impl))
    {}
    ~FullDhtGroups() override = default;

    std::set<GroupName> groups() const override
    { return _impl->groups(); }

    std::set<ItemName> items(const GroupName& gn) const override
    { return _impl->items(gn); }

    void add(const GroupName& gn, const ItemName& in, Cancel& c, asio::yield_context y) override
    { return _impl->add(gn, in, c, y); }

    std::set<GroupName> remove(const ItemName& in) override
    { return _impl->remove(in); }

    void remove_group(const GroupName& gn) override
    { _impl->remove_group(gn); }

private:
    std::unique_ptr<DhtGroupsImpl> _impl;
};

std::unique_ptr<DhtGroups>
ouinet::load_dht_groups( fs::path root_dir
                       , asio::executor ex
                       , Cancel& cancel
                       , yield_context yield)
{
    return std::make_unique<FullDhtGroups>
        (DhtGroupsImpl::load_trusted( std::move(root_dir), std::move(ex)
                                    , cancel, std::move(yield)));
}

class BackedDhtGroups : public FullDhtGroups {
public:
    BackedDhtGroups( std::unique_ptr<DhtGroupsImpl> impl
                   , std::unique_ptr<BaseDhtGroups> fg)
        : FullDhtGroups(std::move(impl))
        , fallback_groups(std::move(fg))
    {}
    ~BackedDhtGroups() override = default;

    std::set<GroupName> groups() const override
    {
        // No `std::set::merge` in C++14,
        // see <https://stackoverflow.com/a/7089642>.
        std::set<GroupName> ret;
        auto groups_ = FullDhtGroups::groups();
        auto fbgroups = fallback_groups->groups();
        std::set_union( groups_.begin(), groups_.end()
                      , fbgroups.begin(), fbgroups.end()
                      , std::inserter(ret, ret.begin()) );
        return ret;
    }

    std::set<ItemName> items(const GroupName& gn) const override
    {
        // No `std::set::merge` in C++14,
        // see <https://stackoverflow.com/a/7089642>.
        std::set<ItemName> ret;
        auto items_ = FullDhtGroups::items(gn);
        auto fbitems = fallback_groups->items(gn);
        std::set_union( items_.begin(), items_.end()
                      , fbitems.begin(), fbitems.end()
                      , std::inserter(ret, ret.begin()) );
        return ret;
    }

    std::set<GroupName> remove(const ItemName& in) override
    {
        auto emptied = FullDhtGroups::remove(in);
        auto fbgroups = fallback_groups->groups();
        // Do not report groups still in fallback as emptied.
        for (auto git = emptied.begin(); git != emptied.end(); ) {
            if (fbgroups.find(*git) != fbgroups.end())
                git = emptied.erase(git);
            else
                git++;
        }
        return emptied;
    }

private:
    std::unique_ptr<BaseDhtGroups> fallback_groups;
};

std::unique_ptr<DhtGroups>
ouinet::load_backed_dht_groups( fs::path root_dir
                              , std::unique_ptr<BaseDhtGroups> fallback_groups
                              , asio::executor ex
                              , Cancel& cancel
                              , yield_context yield)
{
    return std::make_unique<BackedDhtGroups>
        ( DhtGroupsImpl::load_trusted( std::move(root_dir), std::move(ex)
                                     , cancel, std::move(yield))
        , std::move(fallback_groups));
}
