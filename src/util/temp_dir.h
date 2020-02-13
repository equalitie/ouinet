#pragma once

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <boost/system/error_code.hpp>

#include "temp_file.h"

#include "../namespaces.h"

namespace ouinet { namespace util {

class temp_dir {
public:
    // Create a temporary directory named after the given `temp_model` under `dir`.
    // If `keep_on_close(false)`, remove the directory on close.
    static
    boost::optional<temp_dir>
    make( const fs::path& dir
        , const fs::path& model
        , sys::error_code&);

    static
    boost::optional<temp_dir>
    make( const fs::path& dir
        , sys::error_code& ec) {
        return make(dir, default_temp_model, ec);
    }

    static
    boost::optional<temp_dir>
    make(sys::error_code& ec) {
        return make(".", default_temp_model, ec);
    }

public:
    temp_dir(const temp_dir&) = delete;
    temp_dir& operator=(const temp_dir&) = delete;

    temp_dir(temp_dir&&) = default;
    temp_dir& operator=(temp_dir&&) = default;

    ~temp_dir() {
        close();
    }

    const fs::path& path() const { return _path; }
    bool keep_on_close() const { return _keep_on_close; }
    void keep_on_close(bool k) { _keep_on_close = k; }

    void close();

private:
    temp_dir(fs::path path)
        : _path(std::move(path))
    {}

    fs::path _path;
    bool _keep_on_close = true;
};

}} // namespaces
