#include "file.h"

namespace ouinet::ouisync_service {

// static
OuisyncFile OuisyncFile::init(ouisync::File inner, asio::yield_context yield) {
    sys::error_code ec;
    size_t file_size = inner.get_length(yield[ec]);

    if (ec) return or_throw<OuisyncFile>(yield, ec);

    return OuisyncFile (
        yield.get_executor(),
        std::make_shared<State>(
            file_size,
            0, // offset
            false, // closed
            std::make_unique<ouisync::File>(std::move(inner))
        )
    );
}

OuisyncFile OuisyncFile::empty(asio::any_io_executor exec) {
    return OuisyncFile (
        exec,
        std::make_shared<State>(
            0, // file_size
            0, // offset
            false, // closed
            nullptr
        )
    );
}

OuisyncFile::~OuisyncFile() {
    close();
}

} // namespace ouinet::ouisync_service
