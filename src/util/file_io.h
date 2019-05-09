#pragma once

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/filesystem.hpp>

#include "signal.h"
#include "../namespaces.h"
#include "../or_throw.h"

namespace ouinet { namespace util { namespace file_io {

asio::posix::stream_descriptor
open_or_create(asio::io_service& ios, const fs::path&, sys::error_code&);

asio::posix::stream_descriptor
open_readonly(asio::io_service& ios, const fs::path&, sys::error_code&);

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

}}} // namespaces
