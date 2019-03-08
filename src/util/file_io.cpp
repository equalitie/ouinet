#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "file_io.h"

namespace ouinet { namespace util { namespace file_io {

namespace errc = boost::system::errc;
namespace posix = asio::posix;

static
sys::error_code last_error()
{
    return make_error_code(static_cast<errc::errc_t>(errno));
}

void fseek(posix::stream_descriptor& f, size_t pos, sys::error_code& ec)
{
    if (lseek(f.native_handle(), pos, SEEK_SET) == -1) {
        ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
    }
}

size_t current_position(posix::stream_descriptor& f, sys::error_code& ec)
{
    off_t offset = lseek(f.native_handle(), 0, SEEK_CUR);

    if (offset == -1) {
        ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
        return size_t(-1);
    }

    return offset;
}

size_t file_size(posix::stream_descriptor& f, sys::error_code& ec)
{
    auto start_pos = current_position(f, ec);
    if (ec) return size_t(-1);

    if (lseek(f.native_handle(), 0, SEEK_END) == -1) {
        ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
    }

    auto end = current_position(f, ec);
    if (ec) return size_t(-1);

    fseek(f, start_pos, ec);
    if (ec) return size_t(-1);

    return end;
}

size_t file_remaining_size(posix::stream_descriptor& f, sys::error_code& ec)
{
    auto size = file_size(f, ec);
    if (ec) return 0;

    auto pos = current_position(f, ec);
    if (ec) return 0;

    return size - pos;
}

posix::stream_descriptor open( asio::io_service& ios
                             , const fs::path& p
                             , sys::error_code& ec)
{
    int file = ::open(p.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);

    if (file == -1) {
        ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
        return asio::posix::stream_descriptor(ios);
    }

    asio::posix::stream_descriptor f(ios, file);
    fseek(f, 0, ec);

    return f;
}

void truncate( posix::stream_descriptor& f
             , size_t new_length
             , sys::error_code& ec)
{
    if (ftruncate(f.native_handle(), new_length) != 0) {
        ec = last_error();
        if (!ec) ec = make_error_code(errc::no_message);
    }
}

void read( posix::stream_descriptor& f
         , asio::mutable_buffer b
         , Cancel& cancel
         , asio::yield_context yield)
{
    auto cancel_slot = cancel.connect([&] { f.close(); });
    sys::error_code ec;
    asio::async_read(f, b, yield[ec]);
    if (cancel) ec = asio::error::operation_aborted;
    return or_throw(yield, ec);
}

void write( posix::stream_descriptor& f
          , asio::const_buffer b
          , Cancel& cancel
          , asio::yield_context yield)
{
    auto cancel_slot = cancel.connect([&] { f.close(); });
    sys::error_code ec;
    asio::async_write(f, b, yield[ec]);
    if (cancel) ec = asio::error::operation_aborted;
    return or_throw(yield, ec);
}

void remove_file(const fs::path& p)
{
    if (!exists(p)) return;
    assert(is_regular_file(p));
    if (!is_regular_file(p)) return;
    sys::error_code ignored_ec;
    fs::remove(p, ignored_ec);
}

}}} // namespaces
