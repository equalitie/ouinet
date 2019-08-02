#include "http_forward.h"
#include "parse/number.h"
#include "util.h"

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

std::string
process_head( const http::response_header<>& rph, const ProcHeadFunc& rphproc, bool& chunked_out
            , Cancel& cancel, Yield yield) {
    sys::error_code ec;

    auto rph_out(rph);
    rph_out = rphproc(std::move(rph_out), cancel, yield[ec]);
    if (ec) return or_throw<std::string>(yield, ec);

    chunked_out = http::response<http::empty_body>(rph_out).chunked();

    // Write the head as a string to avoid the serializer adding an empty body
    // (which results in a terminating chunk if chunked).
    return util::str(rph_out);
}

http::fields
process_trailers( const http::response_header<>& rph, const ProcTrailFunc& trproc
                , Cancel& cancel, Yield yield) {
    http::fields intrail;
    for (const auto& hdr : http::token_list(rph[http::field::trailer])) {
        auto hit = rph.find(hdr);
        if (hit == rph.end())
            continue;  // missing trailer
        // One would expect `hit->name()` to return `X-Foo` (on such non-standard header),
        // but it returns `<unknown-field>`, so use `hdr` instead
        // to avoid an assertion error in the invocation of `insert`.
        // This may be a bug in Boost Beast.
        intrail.insert(hdr /* hit->name() */, hit->value());
    }
    return trproc(std::move(intrail), cancel, yield);
}

}} // namespaces
