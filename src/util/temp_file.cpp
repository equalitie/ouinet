#include "file_io.h"

#include "temp_file.h"

namespace ouinet { namespace util {

void temp_file::close() {
    // Not completely idempotent:
    // one can set "keep on close" then close and the file remains,
    // then unset "keep on close" then close again and the file is removed.
    _file.close();
    if (!_keep_on_close)
        file_io::remove_file(_path);
}

boost::optional<temp_file>
mktemp( const asio::executor& ex, sys::error_code& ec
      , const fs::path& dir, const fs::path& model)
{
    auto path = dir / fs::unique_path(model, ec);
    if (ec) return boost::none;

    auto file = file_io::open_or_create(ex, path, ec);
    if (ec) return boost::none;

    return temp_file(std::move(file), path);
}

}} // namespaces
