#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include "util/async.h"

namespace ouinet::util::file_io {

namespace errc = boost::system::errc;

std::expected<void, sys::error_code>
fseek(async_file_handle& f, size_t pos)
{
    sys::error_code ec;
    f.seek(static_cast<int64_t>(pos),
           async_file_handle::seek_basis::seek_set,
           ec);
    if (ec) return std::unexpected(ec);
    return {};
}

std::expected<size_t, sys::error_code>
current_position(async_file_handle& f)
{
    sys::error_code ec;
    size_t offset = f.seek(0, async_file_handle::seek_basis::seek_cur, ec);
    if (ec) return std::unexpected(ec);
    return offset;
}

std::expected<size_t, sys::error_code>
file_size(async_file_handle& f)
{
    sys::error_code ec;
    size_t size = f.size(ec);
    if (ec) return std::unexpected(ec);
    return size;
}

std::expected<size_t, sys::error_code>
file_remaining_size(async_file_handle& f)
{
    sys::error_code ec;
    auto size = f.size(ec);
    if (ec) return std::unexpected(ec);

    auto pos = current_position(f);
    if (!pos) return std::unexpected(pos.error());

    return size - *pos;
}

std::expected<async_file_handle, sys::error_code>
open_or_create(const AsioExecutor& exec, const fs::path& p)
{
    sys::error_code ec;
    async_file_handle f{exec};
    f.open(p.string(),
           async_file_handle::create |
           async_file_handle::read_write,
           ec);
    if (ec) return std::unexpected(ec);
    return f;
}

std::expected<async_file_handle, sys::error_code>
open_readonly(const AsioExecutor& exec, const fs::path& p)
{
    sys::error_code ec;
    async_file_handle f{exec};
    f.open(p.string(),
           async_file_handle::read_only,
           ec);
    if (ec) return std::unexpected(ec);
    return f;
}

std::expected<native_handle_t, sys::error_code>
dup_fd(async_file_handle&){
    assert(false && "file_io::dup_fd not implemented yet for Windows");
    std::terminate();
}

std::expected<void, sys::error_code>
truncate(async_file_handle& f, size_t new_length)
{
    sys::error_code ec;
    f.resize(new_length);
    // Move to cursor to the end only when the previous position was in the truncated area
    if (new_length < f.seek(0, async_file_handle::seek_basis::seek_cur, ec))
        f.seek(static_cast<int64_t>(new_length),
               async_file_handle::seek_set,
               ec);
    if (ec) return std::unexpected(ec);
    return {};
}

std::expected<bool, sys::error_code>
check_or_create_directory(const fs::path& dir)
{
    // https://www.boost.org/doc/libs/1_69_0/libs/system/doc/html/system.html#ref_boostsystemerror_code_hpp

    sys::error_code ec;
    namespace errc = boost::system::errc;

    if (fs::exists(dir)) {
        if (!is_directory(dir)) {
            return std::unexpected(make_error_code(errc::not_a_directory));
        }

        return false;
    }
    else {
        if (!create_directories(dir, ec)) {
            if (!ec) ec = make_error_code(errc::operation_not_permitted);
            return std::unexpected(ec);
        }
        assert(is_directory(dir));
        return true;
    }
}

std::expected<void, sys::error_code>
read(async_file_handle& f, asio::mutable_buffer b, Async yield)
{
    auto cancel_slot = yield.cancel_slot([&] { f.close(); });
    auto r = asio::async_read(f, b, yield);
    if (!r) return std::unexpected(r.error());
    return {};
}

std::expected<void, sys::error_code>
write(async_file_handle& f, asio::const_buffer b, Async yield)
{
    auto cancel_slot = yield.cancel_slot([&] { f.close(); });
    auto r = asio::async_write(f, b, yield);
    if (!r) return std::unexpected(r.error());
    return {};
}

std::expected<void, sys::error_code>
remove_file(const fs::path& p)
{
    if (!exists(p)) return {};
    assert(is_regular_file(p));
    if (!is_regular_file(p)) return {};
    sys::error_code ec;
    fs::remove(p, ec);
    if (ec) return std::unexpected(ec);
    return {};
}

} // namespace
