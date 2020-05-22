#include "dht_groups.h"
#include "../logger.h"
#include "../util/file_io.h"
#include "../util/bytes.h"
#include "../util/hash.h"

using namespace ouinet;

#define _LOGPFX "DHT Groups: "
#define _DEBUG(...) LOG_DEBUG(_LOGPFX, __VA_ARGS__)
#define _WARN(...)  LOG_WARN(_LOGPFX, __VA_ARGS__)
#define _ERROR(...) LOG_ERROR(_LOGPFX, __VA_ARGS__)
#define _INFO(...)  LOG_INFO(_LOGPFX, __VA_ARGS__)

using asio::yield_context;
namespace file_io = util::file_io;
using sys::errc::make_error_code;

// https://stackoverflow.com/a/417184/273348
#define MAX_URL_SIZE 2000

DhtGroups::DhtGroups(asio::executor ex, fs::path root_dir, Groups groups)
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
DhtGroups::Group
DhtGroups::load_group( const fs::path dir
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

    for (auto j = fs::directory_iterator(items_dir); j != fs::directory_iterator();) {
        auto i = j; ++j;

        std::string name = read_file(*i, ex, cancel, yield[ec]);

        if (cancel) {
            return or_throw<Group>(yield, asio::error::operation_aborted);
        }

        if (ec) {
            try_remove(*i);
            continue;
        }

        items.insert(name);
    }

    return {std::move(group_name), std::move(items)};
}

std::set<DhtGroups::GroupName> DhtGroups::groups() const
{
    std::set<DhtGroups::GroupName> ret;

    for (auto& group : _groups) {
        ret.insert(group.first);
    }

    return ret;
}

/* static */
std::unique_ptr<DhtGroups> DhtGroups::load( fs::path root_dir
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

    // Note: be careful when refactoring this code, as it seems copying of
    // directory_iterator will create an entangled copy!. I.e. this code will fail:
    //
    // for (auto j = fs::directory_iterator(root_dir); j != fs::directory_iterator();) {
    //     fs::directory_iterator i = j; ++j;
    //     assert(i != j); // O_o
    // }
    for (auto i = fs::directory_iterator(root_dir); i != fs::directory_iterator();) {
        sys::error_code ec;

        if (!fs::is_directory(*i)) {
            _ERROR("Non directory found in '", root_dir, "': '", *i, "'");
            ++i;
            continue;
        }

        auto group = load_group(*i, ex, cancel, yield[ec]);

        if (cancel) return or_throw<Ret>(yield, asio::error::operation_aborted);
        if (ec || group.second.empty()) {
            auto d = *i;
            ++i;
            try_remove(d);
            continue;
        }

        groups.insert(std::move(group));
        ++i;
    }

    return std::unique_ptr<DhtGroups>(new DhtGroups(ex, std::move(root_dir), std::move(groups)));
}

std::string sha1_hex_digest(const std::string& s) {
    return util::bytes::to_hex(util::sha1_digest(s));
}

fs::path
DhtGroups::group_path(const GroupName& group_name)
{
    return _root_dir / sha1_hex_digest(group_name);
}

fs::path
DhtGroups::items_path(const GroupName& group_name)
{
    return group_path(group_name) / "items";
}

fs::path
DhtGroups::item_path(const GroupName& group_name, const ItemName& item_name)
{
    return items_path(group_name) / sha1_hex_digest(item_name);
}

void DhtGroups::add( const GroupName& group_name
                   , const ItemName& item_name
                   , Cancel& cancel
                   , yield_context yield)
{
    _INFO("Adding: ", group_name, " -> ", item_name);
    fs::path group_p = group_path(group_name);

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
}

std::set<DhtGroups::GroupName> DhtGroups::remove(const ItemName& item_name)
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

DhtGroups::~DhtGroups() {
    _lifetime_cancel();
}
