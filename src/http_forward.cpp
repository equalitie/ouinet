#include "http_forward.h"
#include "parse/number.h"

namespace ouinet { namespace detail {

size_t
get_content_length(const http::response_header<>& rph, sys::error_code& ec) {
    boost::string_view cl = rph[http::field::content_length];
    auto length = parse::number<size_t>(cl);  // alters `cl` boundaries
    if (!length) {
        if (rph.version() != 10)
            ec = asio::error::invalid_argument;
        return max_size_t;
    }
    return *length;
}

http::fields
process_trailers( const http::response_header<>& rph, const ProcTrailFunc& trproc
                , Cancel& cancel, Yield yield) {
    http::fields intrail;
    for (const auto& hdr : http::token_list(rph[http::field::trailer])) {
        auto hit = rph.find(hdr);
        if (hit == rph.end())
            continue;  // missing trailer
        intrail.insert(hit->name(), hit->value());
    }
    return trproc(std::move(intrail), cancel, yield);
}

}} // namespaces
