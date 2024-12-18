#ifndef ASYNC_FILE_HANDLE
#define ASYNC_FILE_HANDLE

#ifdef _WIN32
#include <boost/asio/stream_file.hpp>
using async_file_handle = boost::asio::stream_file;
using native_handle_t = HANDLE;
#else
#include <boost/asio/posix/stream_descriptor.hpp>
using async_file_handle = boost::asio::posix::stream_descriptor;
using native_handle_t = int;
#endif

#endif // ASYNC_FILE_HANDLE
