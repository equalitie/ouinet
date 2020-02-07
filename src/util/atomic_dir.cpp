#include "atomic_dir.h"

namespace ouinet { namespace util {

void atomic_dir::commit(sys::error_code& ec) {
    _temp_dir.keep_on_close(true);
    _temp_dir.close();  // required before renaming
    fs::rename(_temp_dir.path(), _path, ec);
    // This allows to retry the commit operation if an error happened,
    // but if the object is destroyed after a failed or no commit,
    // the temporary directory is removed.
    _temp_dir.keep_on_close(false);
}

boost::optional<atomic_dir>
atomic_dir::make( fs::path path
                , const fs::path& temp_model
                , sys::error_code& ec)
{
    auto temp_dir = temp_dir::make(path.parent_path(), temp_model, ec);
    if (ec) return boost::none;
    temp_dir->keep_on_close(false);
    return atomic_dir(std::move(*temp_dir), std::move(path));
}

}} // namespaces
