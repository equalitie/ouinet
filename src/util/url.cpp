#include "url.h"
#include <boost/format.hpp>
#include <boost/regex.hpp>

namespace ouinet::util {

std::string Url::reassemble() const {
    auto url = boost::format("%s://%s%s%s%s%s")
        % scheme % host
        % (port.empty() ? "" : ':' + port)
        % path
        % (query.empty() ? "" : '?' + query)
        % (fragment.empty() ? "" : '#' + fragment);
    return url.str();
}

std::optional<Url> Url::from(const boost::string_view url_s) {
    static const boost::regex urlrx( "^(http|https)://"  // 1: scheme
                                     "([-\\.a-z0-9]+|\\[[:0-9a-f]+\\])"  // 2: host
                                     "(:[0-9]{1,5})?"  // 3: :port (or empty)
                                     "(/[^?#]*)?"  // 4: /path
                                     "(\\?[^#]*)?"  // 5: ?query (or empty)
                                     "(#.*)?"  // 6: #fragment (or empty)
                                   , boost::regex::normal | boost::regex::icase);
    boost::cmatch m;

    if (!boost::regex_match(url_s.begin(), url_s.end(), m, urlrx)) {
        return {};
    }

    return Url {
        m[1],
        m[2],
        m[3].length() > 0 ? std::string(m[3], 1) : "",  // drop colon
        m[4],
        m[5].length() > 0 ? std::string(m[5], 1) : "", // drop qmark
        m[6].length() > 0 ? std::string(m[6], 1) : "", // drop hash
    };
}

} // namespace ouinet::util
