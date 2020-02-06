#include "temp_dir.h"

namespace ouinet { namespace util {

void temp_dir::close() {
    // Not completely idempotent:
    // one can set "keep on close" then close and the directory remains,
    // then unset "keep on close" then close again and the directory is removed.
    if (_keep_on_close) return;
    sys::error_code ec;
    fs::remove_all(_path, ec);
}

boost::optional<temp_dir>
temp_dir::make( const fs::path& dir, const fs::path& model
              , sys::error_code& ec)
{
    auto path = dir / fs::unique_path(model, ec);
    if (ec) return boost::none;

    fs::create_directories(path, ec);
    if (ec) return boost::none;

    return temp_dir(std::move(path));
}

}} // namespaces
