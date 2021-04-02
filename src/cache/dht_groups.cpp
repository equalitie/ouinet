#include "dht_groups.h"
#include "../logger.h"
#include "../util/file_io.h"
#include "../util/bytes.h"
#include "../util/hash.h"

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

class FullDhtGroups : public DhtGroups {
public:
    void add(const GroupName&, const ItemName&, Cancel&, asio::yield_context) override;

    // Remove item from every group it is in. Return groups that became empty
    // as a result.
    std::set<GroupName> remove(const ItemName&) override;

    static std::unique_ptr<DhtGroups> load(fs::path root_dir, asio::executor, Cancel&, asio::yield_context);

    ~FullDhtGroups() override;

    std::set<GroupName> groups() const override;

private:
    using Group  = std::pair<GroupName, std::set<ItemName>>;
    using Groups = std::map<GroupName, std::set<ItemName>>;

    FullDhtGroups(asio::executor, fs::path root_dir, Groups);

    FullDhtGroups(const FullDhtGroups&) = delete;
    FullDhtGroups(FullDhtGroups&&)      = delete;

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

FullDhtGroups::FullDhtGroups(asio::executor ex, fs::path root_dir, Groups groups)
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
                 , path, " ec:", ec.message());
    // The parent directory may be left empty.
}

static std::string read_file(fs::path p, asio::executor ex, Cancel& c, yield_context y)
{
    sys::error_code ec;

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

/* static */
FullDhtGroups::Group
FullDhtGroups::load_group( const fs::path dir
                         , asio::executor ex
                         , Cancel& cancel
                         , yield_context yield)
{
    assert(fs::is_directory(dir));
    sys::error_code ec;

    std::string group_name = read_file(dir/"group_name", ex, cancel, yield[ec]);
    if (ec) return or_throw<Group>(yield, ec);

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
            try_remove(f);
            continue;
        }

        items.insert(name);
    }

    return {std::move(group_name), std::move(items)};
}

std::set<DhtGroups::GroupName> FullDhtGroups::groups() const
{
    std::set<DhtGroups::GroupName> ret;

    for (auto& group : _groups) {
        ret.insert(group.first);
    }

    return ret;
}

/* static */
std::unique_ptr<DhtGroups> FullDhtGroups::load( fs::path root_dir
                                              , asio::executor ex
                                              , Cancel& cancel
                                              , yield_context yield)
{
    using Ret = std::unique_ptr<DhtGroups>;
    namespace err = asio::error;

    Groups groups;

    if (fs::exists(root_dir)) {
        if (!fs::is_directory(root_dir)) {
            _ERROR("Not a directory: '", root_dir, "'");
            return or_throw<Ret>(yield, make_error_code(sys::errc::not_a_directory));
        }
    } else {
        sys::error_code ec;
        fs::create_directories(root_dir, ec);
        if (ec) {
            _ERROR("Failed to create directory: '", root_dir, "' ", ec.message());
            return or_throw<Ret>(yield, ec);
        }
    }

    for (auto f : fs::directory_iterator(root_dir)) {
        sys::error_code ec;

        if (!fs::is_directory(f)) {
            _ERROR("Non directory found in '", root_dir, "': '", f, "'");
            continue;
        }

        auto group = load_group(f, ex, cancel, yield[ec]);

        if (cancel) return or_throw<Ret>(yield, asio::error::operation_aborted);
        if (ec || group.second.empty()) {
            try_remove(f);
            continue;
        }

        groups.insert(std::move(group));
    }

    return std::unique_ptr<DhtGroups>(new FullDhtGroups(ex, std::move(root_dir), std::move(groups)));
}

std::unique_ptr<DhtGroups>
ouinet::load_dht_groups( fs::path root_dir
                       , asio::executor ex
                       , Cancel& cancel
                       , yield_context yield)
{
    return FullDhtGroups::load( std::move(root_dir), std::move(ex)
                              , cancel, std::move(yield));
}

std::string sha1_hex_digest(const std::string& s) {
    return util::bytes::to_hex(util::sha1_digest(s));
}

fs::path
FullDhtGroups::group_path(const GroupName& group_name)
{
    return _root_dir / sha1_hex_digest(group_name);
}

fs::path
FullDhtGroups::items_path(const GroupName& group_name)
{
    return group_path(group_name) / "items";
}

fs::path
FullDhtGroups::item_path(const GroupName& group_name, const ItemName& item_name)
{
    return items_path(group_name) / sha1_hex_digest(item_name);
}

void FullDhtGroups::add( const GroupName& group_name
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
            _ERROR("Failed to create directory for group '", group_name, "': ", ec.message());
            return or_throw(yield, ec);
        }

        auto group_name_f = file_io::open_or_create(_ex, group_p/"group_name", ec);
        if (ec) {
            _ERROR("Failed to create group_name file for group '", group_name, "': ", ec.message());
            try_remove(group_p);
            return or_throw(yield, ec);
        }

        file_io::write(group_name_f, asio::buffer(group_name), cancel, yield[ec]);

        if (ec) {
            if (!cancel) {
                _ERROR("Failed write group name '", group_name, "': ", ec.message());
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
            _ERROR("Failed to create items path '", items_p, "' ", ec.message());
            try_remove(group_p);
            return or_throw(yield, ec);
        }
    }

    auto item_f = file_io::open_or_create(_ex, item_path(group_name, item_name), ec);

    if (ec) {
        _ERROR("Failed to create group item: ", ec.message());
        if (fs::is_empty(items_p)) try_remove(group_p);
        return or_throw(yield, ec);
    }

    file_io::truncate(item_f, 0, ec);

    if (ec) {
        _ERROR("Failed to truncate group item file: ", ec.message());
        if (fs::is_empty(items_p)) try_remove(group_p);
        return or_throw(yield, ec);
    }

    file_io::write(item_f, asio::buffer(item_name), cancel, yield[ec]);

    if (ec) {
        if (!cancel) {
            _ERROR("Failed write to group item: ", ec.message());
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

std::set<DhtGroups::GroupName> FullDhtGroups::remove(const ItemName& item_name)
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

FullDhtGroups::~FullDhtGroups() {
    _lifetime_cancel();
}
