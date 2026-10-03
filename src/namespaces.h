#pragma once

namespace boost {
namespace asio {}
namespace beast {
namespace http {}
} // namespace beast
namespace system {};
namespace filesystem {};
} // namespace boost

namespace ouinet {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace sys = boost::system;
namespace fs = boost::filesystem;

} // namespace ouinet
