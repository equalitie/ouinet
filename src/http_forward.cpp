#include "http_forward.h"

namespace ouinet { namespace detail {

size_t
get_content_length(const http::response_header<>& rph, sys::error_code& ec) {
    auto length = util::parse_num<size_t>( rph[http::field::content_length]
                                         , max_size_t);
    if (length == max_size_t && rph.version() != 10)
        ec = asio::error::invalid_argument;
    return length;
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
