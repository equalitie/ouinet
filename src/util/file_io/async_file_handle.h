#ifndef ASYNC_FILE_HANDLE
#define ASYNC_FILE_HANDLE

#ifdef _WIN32
#include <util/file_io/random_access_handle_extended.hpp>
using async_file_handle = random_access_handle_extended;
using native_handle_t = HANDLE;
#else
#include <boost/asio/posix/stream_descriptor.hpp>
using async_file_handle = boost::asio::posix::stream_descriptor;
using native_handle_t = int;
#endif

#endif // ASYNC_FILE_HANDLE
