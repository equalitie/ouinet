#pragma once

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/core/file_posix.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>

#include "signal.h"
#include "../namespaces.h"
#include "../or_throw.h"

namespace ouinet { namespace util { namespace file_io {

asio::posix::stream_descriptor
open_or_create(asio::io_service& ios, const fs::path&, sys::error_code&);

asio::posix::stream_descriptor
open_readonly(asio::io_service& ios, const fs::path&, sys::error_code&);

// Duplicate the descriptor, see dup(2).
// The descriptor shares offset and flags with that of the original file,
// but it stays open regardless of the original one getting closed,
// so it must be closed separately.
int dup_fd(asio::posix::stream_descriptor&, sys::error_code&);

void fseek(asio::posix::stream_descriptor&, size_t pos, sys::error_code&);

size_t current_position(asio::posix::stream_descriptor&, sys::error_code&);

size_t file_size(asio::posix::stream_descriptor&, sys::error_code&);

size_t file_remaining_size(asio::posix::stream_descriptor&, sys::error_code&);

void truncate( asio::posix::stream_descriptor&
             , size_t new_length
             , sys::error_code&);

void read( asio::posix::stream_descriptor&
         , asio::mutable_buffer
         , Cancel&
         , asio::yield_context);

void write( asio::posix::stream_descriptor&
          , asio::const_buffer
          , Cancel&
          , asio::yield_context);

// Check whether the directory exists, if not, try to create it.
// If the directory doesn't exist nor it can be created, the error
// code is set. Returns true if the directory has been created.
bool check_or_create_directory(const fs::path&, sys::error_code&);

template<typename T>
T read_number( asio::posix::stream_descriptor& f
             , Cancel& cancel
             , asio::yield_context yield)
{
    T num;
    sys::error_code ec;
    // TODO: endianness? (also for writing)
    read(f, asio::buffer(&num, sizeof(num)), cancel, yield[ec]);
    return or_throw<T>(yield, ec, std::move(num));
}

template<typename T>
void write_number( asio::posix::stream_descriptor& f
                 , T num
                 , Cancel& cancel
                 , asio::yield_context yield)
{
    sys::error_code ec;
    // TODO: endianness? (also for reading)
    write(f, asio::buffer(&num, sizeof(num)), cancel, yield[ec]);
    return or_throw(yield, ec);
}

void remove_file(const fs::path& p);


//----------------------------------------------------------------------------
// Temporary and atomic files.

class temp_file {
    friend boost::optional<temp_file>
    mktemp( asio::io_service&, sys::error_code&
          , const fs::path&, const fs::path&);

public:
    using lowest_layer_type = asio::posix::stream_descriptor;

    temp_file(const temp_file&) = delete;
    temp_file& operator=(const temp_file&) = delete;

    temp_file(temp_file&&) = default;
    temp_file& operator=(temp_file&&) = default;

    ~temp_file() {
        close();
    }

    const fs::path& path() const { return _path; }
    bool keep_on_close() const { return _keep_on_close; }
    void keep_on_close(bool k) { _keep_on_close = k; }

    lowest_layer_type& lowest_layer() { return _file; }
    const lowest_layer_type& lowest_layer() const { return _file; }
    auto native_handle() { return _file.native_handle(); }

    // <AsyncReadStream+AsyncWriteStream>
    auto get_executor() { return _file.get_executor(); }
    auto async_read_some(const auto& mb, auto&& t) {
        return _file.async_read_some(mb, std::move(t));
    }
    auto async_write_some(const auto& cb, auto&& t) {
        return _file.async_write_some(cb, std::move(t));
    }
    // </AsyncReadStream+AsyncWriteStream>

    void close();

private:
    temp_file( lowest_layer_type&& file
             , fs::path path)
        : _file(std::move(file))
        , _path(std::move(path))
    {}

    lowest_layer_type _file;
    fs::path _path;
    bool _keep_on_close = true;
};

// Create a temporary file named after the given `temp_model` under `dir`
// and open it for reading and writing.
// Use its `lowest_layer()` to perform I/O.
// If `keep_on_close(false)`, remove the file on close.
boost::optional<temp_file>
mktemp( asio::io_service&, sys::error_code&
      , const fs::path& dir="."
      , const fs::path& model="tmp.%%%%-%%%%-%%%%-%%%%");


class atomic_file {
    friend boost::optional<atomic_file>
    mkatomic( asio::io_service&, sys::error_code&
            , fs::path, const fs::path&);

public:
    using lowest_layer_type = temp_file::lowest_layer_type;

    atomic_file(const atomic_file&) = delete;
    atomic_file& operator=(const atomic_file&) = delete;

    atomic_file(atomic_file&&) = default;
    atomic_file& operator=(atomic_file&&) = default;

    ~atomic_file() {
        // This triggers temporary file removal
        // if it was not previously renamed on commit.
        close();
    }

    const fs::path& path() const { return _path; }

    lowest_layer_type& lowest_layer() { return _temp_file.lowest_layer(); }
    const lowest_layer_type& lowest_layer() const { return _temp_file.lowest_layer(); }
    auto native_handle() { return _temp_file.native_handle(); }

    // <AsyncReadStream+AsyncWriteStream>
    auto get_executor() { return _temp_file.get_executor(); }
    auto async_read_some(const auto& mb, auto&& t) {
        return _temp_file.async_read_some(mb, std::move(t));
    }
    auto async_write_some(const auto& cb, auto&& t) {
        return _temp_file.async_write_some(cb, std::move(t));
    }
    // </AsyncReadStream+AsyncWriteStream>

    void commit(sys::error_code&);

    void close() {
        _temp_file.close();
    }

private:
    atomic_file(temp_file&& file, fs::path path)
        : _temp_file(std::move(file))
        , _path(std::move(path))
    {
        _temp_file.keep_on_close(false);
    }

private:
    temp_file _temp_file;
    fs::path _path;
};

// Create a file to atomically replace `path` once it is committed.
// Storage is backed by a temporary file in the parent directory of `path`
// named after the given `temp_model`.
// Use its `lowest_layer()` to perform I/O.
// If no commit is done or it fails,
// the temporary file is automatically removed.
boost::optional<atomic_file>
mkatomic( asio::io_service&, sys::error_code&
        , fs::path path
        , const fs::path& temp_model="tmp.%%%%-%%%%-%%%%-%%%%");


//----------------------------------------------------------------------------
// `File` implementations.

// A variant of the POSIX file which allows setting a *base offset*,
// so that bytes before that offset are hidden away and not available for
// reading, writing, seeking or computing the file size.
//
// The base offset must always lie within the current boundaries of the file.
//
// Setting the offset multiple times will move the fake beginning of the file
// further towards the end of the file.
class file_posix_with_offset : public beast::file_posix {
    uint64_t base_offset_ = 0;

public:
    uint64_t base_offset() const { return base_offset_; }
    void base_offset(uint64_t offset, sys::error_code& ec) {
        bool too_far = offset > size(ec);
        if (ec) return;
        if (too_far) { ec = asio::error::invalid_argument; return; }
        base_offset_ = offset;
    }

    uint64_t size(sys::error_code& ec) const {
        uint64_t ret = beast::file_posix::size(ec);
        if (ec) return ret;
        return ret - base_offset_;
    }

    uint64_t pos(sys::error_code& ec) const {
        uint64_t ret = beast::file_posix::pos(ec);
        if (ec) return ret;
        return ret - base_offset_;
    }

    void seek(std::uint64_t offset, sys::error_code& ec) {
        beast::file_posix::seek(base_offset_ + offset, ec);
    }
};

}}} // namespaces
