#pragma once

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <boost/system/error_code.hpp>

#include "../namespaces.h"
#include "temp_dir.h"

namespace ouinet { namespace util {

class atomic_dir {
public:
    // Create a directory to atomically replace `path` once it is committed.
    // Storage is backed by a temporary directory in the parent directory of `path`
    // named after the given `temp_model`.
    // Use `temp_path` to gets the temporary directory path.
    // If no commit is done or it fails,
    // the temporary directroy is automatically removed.
    static
    boost::optional<atomic_dir> make( fs::path path
                                    , const fs::path& temp_model
                                    , sys::error_code&);

    static
    boost::optional<atomic_dir> make( fs::path path
                                    , sys::error_code& ec) {
        return make(std::move(path), default_temp_model, ec);
    }

public:
    atomic_dir(const atomic_dir&) = delete;
    atomic_dir& operator=(const atomic_dir&) = delete;

    atomic_dir(atomic_dir&&) = default;
    atomic_dir& operator=(atomic_dir&&) = default;

    ~atomic_dir() {
        // This triggers temporary directrory removal
        // if it was not previously renamed on commit.
        close();
    }

    const fs::path& path() const { return _path; }

    const fs::path& temp_path() const { return _temp_dir.path(); }

    void commit(sys::error_code&);

    void close() {
        _temp_dir.close();
    }

private:
    atomic_dir(temp_dir&& file, fs::path path)
        : _temp_dir(std::move(file))
        , _path(std::move(path))
    {}

private:
    temp_dir _temp_dir;
    fs::path _path;
};

}} // namespaces
