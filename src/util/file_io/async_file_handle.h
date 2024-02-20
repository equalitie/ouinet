#ifndef _WIN32
#include <boost/asio/posix/stream_descriptor.hpp>
using async_file_handle = boost::asio::posix::stream_descriptor;
#endif
