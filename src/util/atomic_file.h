#pragma once

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <boost/system/error_code.hpp>

#include "../namespaces.h"
#include "temp_file.h"

namespace ouinet { namespace util {

class atomic_file {
public:
    // Create a file to atomically replace `path` once it is committed.
    // Storage is backed by a temporary file in the parent directory of `path`
    // named after the given `temp_model`.
    // Use its `lowest_layer()` to perform I/O.
    // If no commit is done or it fails,
    // the temporary file is automatically removed.
    static
    boost::optional<atomic_file> make( const AsioExecutor&
                                     , fs::path
                                     , const fs::path& temp_model
                                     , sys::error_code&);

    static
    boost::optional<atomic_file> make( const AsioExecutor& ex
                                     , fs::path path
                                     , sys::error_code& ec) {
        return make(ex, std::move(path), default_temp_model, ec);
    }

public:
    using lowest_layer_type = temp_file::lowest_layer_type;

    atomic_file(const atomic_file&) = delete;
    atomic_file& operator=(const atomic_file&) = delete;

    atomic_file(atomic_file&&) = default;
    atomic_file& operator=(atomic_file&&) = default;

    ~atomic_file() {
        // This triggers temporary file removal
        // if it was not previously renamed on commit.
        close();
    }

    const fs::path& path() const { return _path; }

    lowest_layer_type& lowest_layer() { return _temp_file.lowest_layer(); }
    const lowest_layer_type& lowest_layer() const { return _temp_file.lowest_layer(); }
    auto native_handle() { return _temp_file.native_handle(); }

    // <AsyncReadStream+AsyncWriteStream>
    auto get_executor() { return _temp_file.get_executor(); }

    template<class MutableBufferSequence, class Token>
    auto async_read_some(const MutableBufferSequence& mb, Token&& t) {
        return _temp_file.async_read_some(mb, std::move(t));
    }

    template<class ConstBufferSequence, class Token>
    auto async_write_some(const ConstBufferSequence& cb, Token&& t) {
        return _temp_file.async_write_some(cb, std::move(t));
    }
    // </AsyncReadStream+AsyncWriteStream>

    void commit(sys::error_code&);

    void close() {
        _temp_file.close();
    }

private:
    atomic_file(temp_file&& file, fs::path path)
        : _temp_file(std::move(file))
        , _path(std::move(path))
    {}

private:
    temp_file _temp_file;
    fs::path _path;
};

}} // namespaces
