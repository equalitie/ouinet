#pragma once

namespace ouinet { namespace cache { namespace detail {

template<class Stream>
void http_store_v0( http_response::AbstractReader& reader, Stream& outf
                  , Cancel cancel, asio::yield_context yield)
{
    // Session flush could be used instead
    // but we want to avoid moving the reader in.
    while (true) {
        sys::error_code ec;
        auto part = reader.async_read_part(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
        if (!part) break;
        part->async_write(outf, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
    }
}

}}} // namespaces
