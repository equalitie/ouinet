#include "http_util.h"

#include <boost/asio/error.hpp>
#include <boost/regex.hpp>
#include <network/uri.hpp>

#include "logger.h"
#include "parse/number.h"
#include "split_string.h"


using namespace std;

namespace beast = boost::beast;
namespace http = beast::http;
namespace posix_time = boost::posix_time;


pair<string, string>
ouinet::util::get_host_port(const http::request<http::string_body>& req)
{
    auto target = req.target();
    auto defport = (target.starts_with("https:") || target.starts_with("wss:"))
                 ? "443"
                 : "80";

    auto hp = (req.method() == http::verb::connect)
            ? target
            : req[http::field::host];

    if (hp.empty() && req.version() == 10) {
        // HTTP/1.0 proxy client with no ``Host:``, use URI.
        network::uri uri(target.to_string());
        return make_pair( uri.host().to_string()
                        , (uri.has_port() ? uri.port().to_string() : defport));
    }

    auto host_port = util::split_ep(hp);
    return make_pair( host_port.first.to_string()
                    , host_port.second.empty() ? defport : host_port.second.to_string());
}

boost::optional<ouinet::util::HttpResponseByteRange>
ouinet::util::HttpResponseByteRange::parse(boost::string_view s)
{
    static const boost::regex range_rx("^bytes ([0-9]+)-([0-9]+)/([0-9]+|\\*)$");
    boost::cmatch m;
    if (!boost::regex_match(s.begin(), s.end(), m, range_rx))
        return boost::none;

    // Get values, check for overflows.
    s.remove_prefix(m.position(1));
    auto first = parse::number<size_t>(s);
    if (!first) return boost::none;
    s.remove_prefix(1);  // '-'
    auto last = parse::number<size_t>(s);
    if (!last) return boost::none;
    s.remove_prefix(1);  // '/'
    auto length = parse::number<size_t>(s);
    if (m[3] != "*" && !length) return boost::none;

    if ( (*last < *first)
       || (length && *last >= *length))
        return boost::none;  // off-limits

    return ouinet::util::HttpResponseByteRange{*first, *last, std::move(length)};
}

bool
ouinet::util::HttpResponseByteRange::matches_length(size_t s) const {
    if (!length) return false;  // it should have a value
    if (*length != s) return false;  // values do not match
    return true;
}

bool
ouinet::util::HttpResponseByteRange::matches_length(boost::string_view ls) const {
    auto s = parse::number<size_t>(ls);
    if (s) return matches_length(*s);
    if (length) return false;  // length should be "*"
    return true;  // length is "*"
}

std::ostream&
ouinet::util::operator<<( std::ostream& os
                        , const ouinet::util::HttpResponseByteRange& br)
{
    os << "bytes " << br.first << '-' << br.last << '/';
    if (br.length) return os << *(br.length);
    return os << '*';
}

boost::optional<std::vector<ouinet::util::HttpRequestByteRange>>
ouinet::util::HttpRequestByteRange::parse(boost::string_view s)
{
    using Ranges = std::vector<ouinet::util::HttpRequestByteRange>;

    static auto trim_ws = [](boost::string_view& s) {
        while (!s.empty() && s[0] == ' ') s.remove_prefix(1);
    };

    static auto consume = [](boost::string_view& s, boost::string_view what) -> bool {
        if (!s.starts_with(what)) return false;
        s.remove_prefix(what.size());
        return true;
    };

    trim_ws(s);
    if (!consume(s, "bytes")) return boost::none;
    trim_ws(s);
    if (!consume(s, "=")) return boost::none;
    trim_ws(s);

    Ranges ranges;

    while (true) {
        auto first = parse::number<size_t>(s);
        if (!first) return boost::none;
        trim_ws(s);
        if (!consume(s, "-")) return boost::none;
        trim_ws(s);
        auto last = parse::number<size_t>(s);
        if (!last) return boost::none;
        ranges.push_back({*first, *last});
        trim_ws(s);
        if (!consume(s, ",")) break;
        trim_ws(s);
    }

    return ranges;
}

#ifdef __SANITIZE_ADDRESS__
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

    if (g.tm_year == 0) return boost::none;

    g.tm_year -= 1900;
    t = timegm (& g);

    return posix_time::from_time_t(t);
}
#endif

posix_time::ptime
ouinet::util::parse_date(beast::string_view s)
{
    namespace bt = boost::posix_time;

    // Trim quotes from the beginning
    while (s.starts_with('"') || s.starts_with(' ')) s.remove_prefix(1);

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

    ostringstream ss;

    ss.imbue(std::locale(std::locale::classic(), facet));
    ss << date;
    return ss.str();
}

boost::string_view
ouinet::util::http_injection_field( const http::response_header<>& rsh
                                  , const string& field)
{
    auto ih = rsh[http_::response_injection_hdr];
    if (ih.empty()) return {};  // missing header
    for (auto item : SplitString(ih, ',')) {
        auto k_v = split_string_pair(item, '=');
        if (k_v.first != field) continue;
        return k_v.second;
    }
    return {};  // missing id item in header
}

boost::optional<http::response<http::empty_body>>
ouinet::util::detail::http_proto_version_error( unsigned rq_version
                                              , beast::string_view oui_version
                                              , beast::string_view server_string)
{
    unsigned version = 0;

    if (auto opt_version = parse::number<unsigned>(oui_version)) {
        version = *opt_version;
    }

    unsigned supported_version = -1;

    beast::string_view supported_version_s = http_::protocol_version_hdr_current;
    if (auto opt_sv = parse::number<unsigned>(supported_version_s)) {
        supported_version = *opt_sv;
    }

    assert(supported_version != (unsigned) -1);

    if (version == supported_version) {
        return boost::none;
    }

    http::response<http::empty_body> res{http::status::bad_request, rq_version};
    // Set the response's protocol version to that of the request
    // (so that the requester does try to parse the response)
    // and add an error message which should be accepted regardless of that version.
    res.set(http_::protocol_version_hdr, oui_version);
    res.set(http::field::server, server_string);
    res.keep_alive(false);

    if (version < supported_version) {
        res.set( http_::response_error_hdr
               , http_::response_error_hdr_version_too_low);
    }
    else if (version > supported_version) {
        res.set( http_::response_error_hdr
               , http_::response_error_hdr_version_too_high);
    }

    res.prepare_payload();  // avoid consumer getting stuck waiting for body
    return res;
}

bool
ouinet::util::detail::http_proto_version_check_trusted( boost::string_view proto_vs
                                                      , unsigned& newest_proto_seen)
{
    if (!boost::regex_match( proto_vs.begin(), proto_vs.end()
                           , http_::protocol_version_rx))
        return false;  // malformed version header

    auto proto_vn = *(parse::number<unsigned>(proto_vs));
    if (proto_vn > newest_proto_seen) {
        LOG_WARN( "Found new protocol version in trusted source: "
                , proto_vn, " > ", http_::protocol_version_current);
        newest_proto_seen = proto_vn;  // saw a newest protocol in the wild
    }

    return (proto_vn == http_::protocol_version_current);  // unsupported version?
}

std::string
ouinet::util::detail::http_host_header(const std::string& host, const std::string& port)
{
    if (host.empty()) return {};  // error
    if (port.empty()) return host;
    if (host.find(':') != string::npos) return '[' + host + "]:" + port;  // IPv6
    return host + ':' + port;
}


http::response_header<>
ouinet::util::to_cache_response(http::response_header<> rs, sys::error_code& ec) {
    // Only identity and chunked transfer encodings are supported.
    // (Also canonical requests do not have a `TE:` header.)
    auto rs_te = rs[http::field::transfer_encoding];
    if (!rs_te.empty() && !boost::iequals(rs_te, "chunked")) {
        ec = asio::error::invalid_argument;
        return rs;
    }

    rs = remove_ouinet_fields(move(rs));
    // TODO: Handle `Trailer:` properly.
    // TODO: This list was created by going through some 100 responses from
    // bbc.com. Careful selection from all possible (standard) fields is
    // needed.
    return filter_fields( move(rs)
                        , http::field::server
                        , http::field::retry_after
                        , http::field::content_length
                        , http::field::content_type
                        , http::field::content_encoding
                        , http::field::content_language
                        , http::field::digest
                        , http::field::transfer_encoding
                        , http::field::accept_ranges
                        , http::field::etag
                        , http::field::age
                        , http::field::date
                        , http::field::expires
                        , http::field::via
                        , http::field::vary
                        , http::field::location
                        , http::field::cache_control
                        , http::field::warning
                        , http::field::last_modified
                        // # CORS response headers (following <https://fetch.spec.whatwg.org/#http-responses>)
                        , http::field::access_control_allow_origin  // origins the response may be shared with
                        // A request which caused a response with ``Access-Control-Allow-Credentials: true``
                        // probably carried authentication tokens and it should not have been cached anyway,
                        // however a server may erroneously include it for requests not using credentials,
                        // and we do not want to block them.
                        // See <https://stackoverflow.com/a/24689738> for an explanation of the header.
                        , http::field::access_control_allow_credentials  // resp to req w/credentials may be shared
                        // These response headers should only appear in
                        // responses to pre-flight (OPTIONS) requests, which should not be cached.
                        // However, some servers include them as part of responses to GET requests,
                        // so include them since they are not problematic either.
                        , http::field::access_control_allow_methods  // methods allowed in CORS request
                        , http::field::access_control_allow_headers  // headers allowed in CORS request
                        , http::field::access_control_max_age  // expiration of pre-flight response info
                        , http::field::access_control_expose_headers  // headers of response to be exposed
                        );
}

http::fields
ouinet::util::to_cache_trailer(http::fields rst)
{
    // TODO: Handle properly.
    rst.clear();
    return rst;
}
