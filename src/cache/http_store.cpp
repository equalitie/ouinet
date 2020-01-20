#include "http_store.h"

#include "../or_throw.h"

namespace ouinet { namespace cache {

void
http_store( http_response::AbstractReader& reader, const fs::path&
          , Cancel cancel, asio::yield_context yield)
{
    // TODO: implement
    sys::error_code ec;
    while (!ec) {  // just consume input
        auto part = reader.async_read_part(cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
        if (!part) break;
    }
}

}} // namespaces
