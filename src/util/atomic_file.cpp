#include "atomic_file.h"

namespace ouinet { namespace util {

void atomic_file::commit(sys::error_code& ec) {
    _temp_file.keep_on_close(true);
    _temp_file.close();  // required before renaming
    fs::rename(_temp_file.path(), _path, ec);
    // This allows to retry the commit operation if an error happened,
    // but if the object is destroyed after a failed or no commit,
    // the temporary file is removed.
    _temp_file.keep_on_close(false);
}

boost::optional<atomic_file>
mkatomic( asio::io_service& ios, sys::error_code& ec
        , fs::path path, const fs::path& temp_model)
{
    auto temp_file = mktemp(ios, ec, path.parent_path(), temp_model);
    if (ec) return boost::none;

    return atomic_file(std::move(*temp_file), std::move(path));
}

}} // namespaces
