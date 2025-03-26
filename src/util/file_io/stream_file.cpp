#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>


namespace ouinet { namespace util { namespace file_io {

namespace errc = boost::system::errc;

void
fseek(async_file_handle& f, size_t pos, sys::error_code& ec)
{
    f.seek(static_cast<int64_t>(pos),
           async_file_handle::seek_basis::seek_set,
           ec);
}

size_t
current_position(async_file_handle& f, sys::error_code& ec)
{
    size_t offset = f.seek(0, async_file_handle::seek_basis::seek_cur, ec);
    return offset;
}

size_t
file_size(async_file_handle& f, sys::error_code& ec)
{
    return f.size(ec);
}

size_t
file_remaining_size(async_file_handle& f, sys::error_code& ec)
{
    auto size = f.size(ec);
    if (ec) return 0;

    auto pos = current_position(f, ec);
    if (ec) return 0;

    return size - pos;
}

async_file_handle
open_or_create( const AsioExecutor& exec
              , const fs::path& p
              , sys::error_code& ec)
{
    async_file_handle f{exec};
    f.open(p.string(),
           async_file_handle::create |
           async_file_handle::read_write,
           ec);

    return f;
}

async_file_handle
open_readonly( const AsioExecutor& exec
             , const fs::path& p
             , sys::error_code& ec)
{
    async_file_handle f{exec};
    f.open(p.string(),
           async_file_handle::read_only,
           ec);
    return  f;
}

void
truncate( async_file_handle& f
        , size_t new_length
        , sys::error_code& ec)
{
    f.resize(new_length);
    f.seek(static_cast<int64_t>(new_length),
           async_file_handle::seek_set,
           ec);
}

bool
check_or_create_directory(const fs::path& dir, sys::error_code& ec)
{
    // https://www.boost.org/doc/libs/1_69_0/libs/system/doc/html/system.html#ref_boostsystemerror_code_hpp

    namespace errc = boost::system::errc;

    if (fs::exists(dir)) {
        if (!is_directory(dir)) {
            ec = make_error_code(errc::not_a_directory);
            return false;
        }

        return false;
    }
    else {
        if (!create_directories(dir, ec)) {
            if (!ec) ec = make_error_code(errc::operation_not_permitted);
            return false;
        }
        assert(is_directory(dir));
        return true;
    }
}

void
read( async_file_handle& f
    , asio::mutable_buffer b
    , Cancel& cancel
    , asio::yield_context yield)
{
    auto cancel_slot = cancel.connect([&] { f.close(); });
    sys::error_code ec;
    asio::async_read(f, b, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec);
}

void
write( async_file_handle& f
     , asio::const_buffer b
     , Cancel& cancel
     , asio::yield_context yield)
{
    auto cancel_slot = cancel.connect([&] { f.close(); });
    sys::error_code ec;
    asio::async_write(f, b, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec)
}

void
remove_file(const fs::path& p)
{
    if (!exists(p)) return;
    assert(is_regular_file(p));
    if (!is_regular_file(p)) return;
    sys::error_code ignored_ec;
    fs::remove(p, ignored_ec);
}

}}} // namespaces
