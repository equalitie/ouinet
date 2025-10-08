#include "file.h"

namespace ouinet::ouisync_service {

// static
OuisyncFile OuisyncFile::init(ouisync::File inner, asio::yield_context yield) {
    sys::error_code ec;
    size_t file_size = inner.get_length(yield[ec]);

    if (ec) return or_throw<OuisyncFile>(yield, ec);

    return OuisyncFile (
        yield.get_executor(),
        file_size,
        std::move(inner)
    );
}

} // namespace ouinet::ouisync_service
