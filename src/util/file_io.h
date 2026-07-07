#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/filesystem.hpp>

#include "cancel.h"
#include "../namespaces.h"
#include "../or_throw.h"
#include "../util/executor.h"
#include "../util/file_io/async_file_handle.h"
#include "api.h"
#include "async.h"

#include <expected>

namespace ouinet::util::file_io {

OUINET_COMMON_API
[[nodiscard]]
std::expected<async_file_handle, sys::error_code>
open_or_create(const AsioExecutor&, const fs::path&);

OUINET_COMMON_API
[[nodiscard]]
std::expected<async_file_handle, sys::error_code>
open_readonly(const AsioExecutor&, const fs::path&);

// Duplicate the descriptor, see dup(2).
// The descriptor shares offset and flags with that of the original file,
// but it stays open regardless of the original one getting closed,
// so it must be closed separately.
OUINET_COMMON_API
[[nodiscard]]
std::expected<native_handle_t, sys::error_code>
dup_fd(async_file_handle&);

OUINET_COMMON_API
[[nodiscard]]
std::expected<void, sys::error_code>
fseek(async_file_handle&, size_t pos);

OUINET_COMMON_API
[[nodiscard]]
std::expected<size_t, sys::error_code>
current_position(async_file_handle&);

OUINET_COMMON_API
[[nodiscard]]
std::expected<size_t, sys::error_code>
file_size(async_file_handle&);

OUINET_COMMON_API
[[nodiscard]]
std::expected<size_t, sys::error_code>
file_remaining_size(async_file_handle&);

OUINET_COMMON_API
[[nodiscard]]
std::expected<void, sys::error_code>
truncate(async_file_handle&, size_t new_length);

OUINET_COMMON_API
[[nodiscard]]
std::expected<void, sys::error_code>
read(async_file_handle&, asio::mutable_buffer, Async);

OUINET_COMMON_API
[[nodiscard]]
std::expected<void, sys::error_code>
write(async_file_handle&, asio::const_buffer, Async);

// Check whether the directory exists, if not, try to create it.
// If the directory doesn't exist nor it can be created, the error
// code is set. Returns true if the directory has been created.
OUINET_COMMON_API
[[nodiscard]]
std::expected<bool, sys::error_code>
check_or_create_directory(const fs::path&);

template<typename T>
[[nodiscard]]
std::expected<T, sys::error_code>
read_number(async_file_handle& f, Async yield)
{
    T num;
    // TODO: endianness? (also for writing)
    auto r = read(f, asio::buffer(&num, sizeof(num)), yield);
    if (!r) return std::unexpected(r.error());
    return num;
}

template<typename T>
[[nodiscard]]
std::expected<T, sys::error_code>
write_number(async_file_handle& f, T num, Async yield)
{
    // TODO: endianness? (also for reading)
    auto r = write(f, asio::buffer(&num, sizeof(num)), yield);
    if (!r) return std::unexpected(r.error());
    return {};
}

OUINET_COMMON_API
[[nodiscard]]
std::expected<void, sys::error_code>
remove_file(const fs::path& p);

} // namespace
