#pragma once

#include <string>
#include <optional>
#include <ostream>
#include <boost/utility/string_view.hpp>
#include "api.h"

namespace ouinet::util {

struct OUINET_COMMON_API Url {
    // Uniform Resource Identifier (URI): Generic Syntax
    // https://www.ietf.org/rfc/rfc3986.txt

    //      foo://example.com:8042/over/there?name=ferret#nose
    //      \_/   \______________/\_________/ \_________/ \__/
    //       |           |            |            |        |
    //    scheme     authority       path        query   fragment
    //
    // authority = [ userinfo "@" ] host [ ":" port ]

    std::string scheme;
    std::string host;
    std::string port;      // maybe empty
    std::string path;      // maybe empty
    std::string query;     // maybe empty
    std::string fragment;  // maybe empty

    static
    std::optional<Url> from(const boost::string_view url);

    // Rebuild the URL, dropping port, query and fragment if empty.
    std::string reassemble() const;

    std::string host_and_port() const {
        if (port.empty()) {
            return host;
        } else {
            return host + ':' + port;
        }
    }

    friend std::ostream& operator<<(std::ostream&, const Url&);
};

} // namespace ouinet::util
