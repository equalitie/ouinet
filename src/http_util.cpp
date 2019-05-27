#include "http_util.h"

#include <boost/asio/error.hpp>
#include <network/uri.hpp>


using namespace std;

namespace beast = boost::beast;
namespace posix_time = boost::posix_time;


pair<string, string>
ouinet::util::get_host_port(const http::request<http::string_body>& req)
{
    auto target = req.target();
    string host, port;
    auto defport = (target.starts_with("https:") || target.starts_with("wss:"))
                 ? "443"
                 : "80";

    auto hp = (req.method() == http::verb::connect)
            ? target
            : req[http::field::host];
    auto cpos = hp.rfind(':');

    if (hp.empty() && req.version() == 10) {
        // HTTP/1.0 proxy client with no ``Host:``, use URI.
        network::uri uri(target.to_string());
        host = uri.host().to_string();
        port = uri.has_port() ? uri.port().to_string() : defport;
    } else if (cpos == string::npos || hp[hp.length() - 1] == ']') {
        // ``Host:`` header present, no port.
        host = hp.to_string();
        port = defport;
    } else {
        // ``Host:`` header present, explicit port (or CONNECT request).
        host = hp.substr(0, cpos).to_string();
        port = hp.substr(cpos + 1).to_string();
    }

    // Remove brackets from IPv6 hosts.
    if (host[0] == '[')
        host = host.substr(1, host.length() - 2);

    return make_pair(host, port);
}

static
boost::optional<posix_time::ptime>
parse_date_rfc1123(beast::string_view s)
{
    static const char * months[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    struct tm g = {0};
    char M[4];
    time_t t;
    int i;

    char buf[128];

    if (s.size() >= sizeof(buf)) return boost::none;

    memcpy(buf, s.data(), s.size());
    buf[s.size()] = 0;

    sscanf(buf, "%*[a-zA-Z,] %d %3s %d %d:%d:%d",
	   & g.tm_mday, M, & g.tm_year,
	   & g.tm_hour, & g.tm_min, & g.tm_sec);
    for (i = 0; i < 12; i++) {
	    if (strncmp (M, months[i], 3) == 0) {
	        g.tm_mon = i;
	        break;
	    }
    }
    g.tm_year -= 1900;
    t = timegm (& g);

    return posix_time::from_time_t(t);
}

posix_time::ptime
ouinet::util::parse_date(beast::string_view s)
{
    namespace bt = boost::posix_time;

    // The date parsing code below internally throws and catches exceptions.
    // This confuses the address sanitizer when combined with Boost.Coroutine
    // and causes the app exit with false positive log from Asan.
#   ifdef __SANITIZE_ADDRESS__
    {
        auto t = parse_date_rfc1123(s);
        if (!t) return bt::ptime();
        return *t;
    }
#   endif

    // Trim quotes from the beginning
    while (s.starts_with('"')) s.remove_prefix(1);

    static const auto format = [](const char* fmt) {
        using std::locale;
        return locale(locale::classic(), new bt::time_input_facet(fmt));
    };

    // https://www.w3.org/Protocols/rfc2616/rfc2616-sec3.html#sec3.3

    // Format spec:
    // http://www.boost.org/doc/libs/1_60_0/doc/html/date_time/date_time_io.html
    static const std::locale formats[] = {
        format("%a, %d %b %Y %H:%M:%S"),
        format("%A, %d-%b-%y %H:%M:%S"),
        // TODO: ANSI C's format not done yet because Boost doesn't seem
        // to support parsing days of month in 1-digit format.
    };

    const size_t formats_n = sizeof(formats)/sizeof(formats[0]);

    bt::ptime pt;

    // https://stackoverflow.com/a/13059195/273348
    struct membuf: std::streambuf {
        membuf(char const* base, size_t size) {
            char* p(const_cast<char*>(base));
            this->setg(p, p, p + size);
        }
    };

    struct imemstream: virtual membuf, std::istream {
        imemstream(beast::string_view s)
            : membuf(s.data(), s.size())
            , std::istream(static_cast<std::streambuf*>(this)) {
        }
    };

    for(size_t i=0; i<formats_n; ++i) {
        imemstream is(s);
        is.istream::imbue(formats[i]);
        is >> pt;
        if(pt != bt::ptime()) return pt;
    }

    return pt;
}

string
ouinet::util::format_date(posix_time::ptime date)
{
    posix_time::time_facet* facet = new posix_time::time_facet();

    facet->format("%a, %d %b %Y %H:%M:%S");

    stringstream ss;

    ss.imbue(std::locale(std::locale::classic(), facet));
    ss << date;
    return ss.str();
}
