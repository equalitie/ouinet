#pragma once

namespace boost {
    namespace asio  {}
    namespace beast { namespace http {} }
    namespace system {};
    namespace filesystem {};
}

namespace ouinet {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;
namespace sys   = boost::system;
namespace fs    = boost::filesystem;

} // ouinet namespace
