#pragma once

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <boost/system/error_code.hpp>

#include "../namespaces.h"

namespace ouinet { namespace util {

static const fs::path default_temp_model{"tmp.%%%%-%%%%-%%%%-%%%%"};

class temp_file {
public:
    // Create a temporary file named after the given `temp_model` under `dir`
    // and open it for reading and writing.
    // Use its `lowest_layer()` to perform I/O.
    // If `keep_on_close(false)`, remove the file on close.
    static
    boost::optional<temp_file>
    make( const asio::executor&
        , const fs::path& dir
        , const fs::path& model
        , sys::error_code&);

    static
    boost::optional<temp_file>
    make( const asio::executor& ex
        , const fs::path& dir
        , sys::error_code& ec) {
        return make(ex, dir, default_temp_model, ec);
    }

    static
    boost::optional<temp_file>
    make( const asio::executor& ex
        , sys::error_code& ec) {
        return make(ex, ".", default_temp_model, ec);
    }

public:
    using lowest_layer_type = asio::posix::stream_descriptor;

    temp_file(const temp_file&) = delete;
    temp_file& operator=(const temp_file&) = delete;

    temp_file(temp_file&&) = default;
    temp_file& operator=(temp_file&&) = default;

    ~temp_file() {
        close();
    }

    const fs::path& path() const { return _path; }
    bool keep_on_close() const { return _keep_on_close; }
    void keep_on_close(bool k) { _keep_on_close = k; }

    lowest_layer_type& lowest_layer() { return _file; }
    const lowest_layer_type& lowest_layer() const { return _file; }
    auto native_handle() { return _file.native_handle(); }

    // <AsyncReadStream+AsyncWriteStream>
    auto get_executor() { return _file.get_executor(); }

    template<class MutableBufferSequence, class Token>
    auto async_read_some(const MutableBufferSequence& mb, Token&& t) {
        return _file.async_read_some(mb, std::move(t));
    }

    template<class ConstBufferSequence, class Token>
    auto async_write_some(const ConstBufferSequence& cb, Token&& t) {
        return _file.async_write_some(cb, std::move(t));
    }
    // </AsyncReadStream+AsyncWriteStream>

    void close();

private:
    temp_file( lowest_layer_type&& file
             , fs::path path)
        : _file(std::move(file))
        , _path(std::move(path))
    {}

    lowest_layer_type _file;
    fs::path _path;
    bool _keep_on_close = true;
};

}} // namespaces
