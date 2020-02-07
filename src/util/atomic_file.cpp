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
atomic_file::make( const asio::executor& ex
                 , fs::path path
                 , const fs::path& temp_model
                 , sys::error_code& ec)
{
    auto temp_file = temp_file::make(ex, path.parent_path(), temp_model, ec);
    if (ec) return boost::none;
    temp_file->keep_on_close(false);
    return atomic_file(std::move(*temp_file), std::move(path));
}

}} // namespaces
