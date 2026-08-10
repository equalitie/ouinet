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

std::expected<atomic_file, sys::error_code>
atomic_file::make( const AsioExecutor& ex
                 , fs::path path
                 , const fs::path& temp_model)
{
    auto temp_file = temp_file::make(ex, path.parent_path(), temp_model);
    if (!temp_file) return std::unexpected(temp_file.error());
    temp_file->keep_on_close(false);
    return atomic_file(std::move(*temp_file), std::move(path));
}

}} // namespaces
