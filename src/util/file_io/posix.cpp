#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include "util/async.h"

namespace ouinet::util::file_io {

namespace errc = boost::system::errc;

static
sys::error_code
last_error()
{
    return make_error_code(static_cast<errc::errc_t>(errno));
}

std::expected<void, sys::error_code>
fseek(async_file_handle& f, size_t pos)
{
    if (lseek(f.native_handle(), pos, SEEK_SET) == -1) {
        sys::error_code ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
        return std::unexpected(ec);
    }
    return {};
}

std::expected<size_t, sys::error_code>
current_position(async_file_handle& f)
{
    off_t offset = lseek(f.native_handle(), 0, SEEK_CUR);

    if (offset == -1) {
        sys::error_code ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
        return std::unexpected(ec);
    }

    return offset;
}

std::expected<size_t, sys::error_code>
file_size(async_file_handle& f)
{
    auto start_pos = current_position(f);
    if (!start_pos) return std::unexpected(start_pos.error());

    if (lseek(f.native_handle(), 0, SEEK_END) == -1) {
        sys::error_code ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
        return std::unexpected(ec);
    }

    auto end = current_position(f);
    if (!end) return std::unexpected(end.error());

    sys::error_code ec;
    auto r = fseek(f, *start_pos);
    if (!r) return std::unexpected(r.error());

    return *end;
}

std::expected<size_t, sys::error_code>
file_remaining_size(async_file_handle& f)
{
    auto size = file_size(f);
    if (!size) return std::unexpected(size.error());

    auto pos = current_position(f);
    if (!pos) return std::unexpected(pos.error());

    return *size - *pos;
}

static
std::expected<async_file_handle, sys::error_code>
open(int file, const AsioExecutor& exec)
{
    if (file == -1) {
        sys::error_code ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
        return std::unexpected(ec);
    }

    async_file_handle f(exec, file);
    auto r = fseek(f, 0);
    if (!r) return std::unexpected(r.error());

    return f;
}

std::expected<async_file_handle, sys::error_code>
open_or_create( const AsioExecutor& exec
              , const fs::path& p)
{
    int file = ::open(p.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    return open(file, exec);
}

std::expected<async_file_handle, sys::error_code>
open_readonly( const AsioExecutor& exec
             , const fs::path& p)
{
    int file = ::open(p.c_str(), O_RDONLY);
    return open(file, exec);
}

std::expected<int, sys::error_code>
dup_fd(async_file_handle& f)
{
    int file = ::dup(f.native_handle());
    if (file == -1) {
        sys::error_code ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
        return std::unexpected(ec);
    }
    return file;
}

std::expected<void, sys::error_code>
truncate(async_file_handle& f, size_t new_length)
{
    if (ftruncate(f.native_handle(), new_length) != 0) {
        sys::error_code ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
        return std::unexpected(ec);
    }
    return fseek(f, new_length);
}

std::expected<bool, sys::error_code>
check_or_create_directory(const fs::path& dir)
{
    // https://www.boost.org/doc/libs/1_69_0/libs/system/doc/html/system.html#ref_boostsystemerror_code_hpp

    namespace errc = boost::system::errc;

    if (fs::exists(dir)) {
        if (!is_directory(dir)) {
            return std::unexpected(make_error_code(errc::not_a_directory));
        }

        return false;
    }
    else {
        sys::error_code ec;
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
