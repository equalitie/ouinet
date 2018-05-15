#pragma once

#include "namespaces.h"

#include <unistd.h>  // for getpid()
#include <fstream>
#include <string>

#include <boost/asio/ip/tcp.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
// Only available in Boost >= 1.64.0.
////#include <boost/process/environment.hpp>

namespace ouinet { namespace util {

inline
asio::ip::tcp::endpoint
parse_tcp_endpoint(const std::string& s, sys::error_code& ec)
{
    using namespace std;
    auto pos = s.find(':');

    ec = sys::error_code();

    if (pos == string::npos) {
        ec = asio::error::invalid_argument;
        return asio::ip::tcp::endpoint();
    }

    auto addr = asio::ip::address::from_string(s.substr(0, pos));

    auto pb = s.c_str() + pos + 1;
    uint16_t port = std::atoi(pb);

    if (port == 0 && !(*pb == '0' && *(pb+1) == 0)) {
        ec = asio::error::invalid_argument;
        return asio::ip::tcp::endpoint();
    }

    return asio::ip::tcp::endpoint(move(addr), port);
}

inline
asio::ip::tcp::endpoint
parse_tcp_endpoint(const std::string& s)
{
    sys::error_code ec;
    auto ep = parse_tcp_endpoint(s, ec);
    if (ec) throw sys::system_error(ec);
    return ep;
}

///////////////////////////////////////////////////////////////////////////////
namespace detail {
inline
std::string str_impl(std::stringstream& ss) {
    return ss.str();
}

template<class Arg, class... Args>
inline
std::string str_impl(std::stringstream& ss, Arg&& arg, Args&&... args) {
    ss << arg;
    return str_impl(ss, std::forward<Args>(args)...);
}

} // detail namespace

template<class... Args>
inline
std::string str(Args&&... args) {
    std::stringstream ss;
    return detail::str_impl(ss, std::forward<Args>(args)...);
}

///////////////////////////////////////////////////////////////////////////////
// Write a small file at the given `path` with a `line` of content.
// If existing, truncate it.
inline
void create_state_file(const boost::filesystem::path& path, const std::string& line) {
    std::fstream fs(path.native(), std::fstream::out | std::fstream::trunc);
    fs << line << std::endl;
    fs.close();
}

///////////////////////////////////////////////////////////////////////////////
class PidFile {
    public:
        PidFile(const boost::filesystem::path& path) : pid_path(path) {
            // Only available in Boost >= 1.64.0.
            ////auto pid = boost::this_process::get_pid();
            // TODO: Check if this works under Windows (it is declared obsolete).
            auto pid = ::getpid();
            create_state_file(pid_path, std::to_string(pid));
        }

        ~PidFile() {
            try {
                remove(pid_path);
            } catch (...) {
            }
        }
    private:
        boost::filesystem::path pid_path;
};

///////////////////////////////////////////////////////////////////////////////

}} // ouinet::util namespace
