#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <boost/system/error_code.hpp>

#include "../namespaces.h"

namespace ouinet { namespace util {

class temp_file {
    friend boost::optional<temp_file>
    mktemp( asio::io_service&, sys::error_code&
          , const fs::path&, const fs::path&);

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

// Create a temporary file named after the given `temp_model` under `dir`
// and open it for reading and writing.
// Use its `lowest_layer()` to perform I/O.
// If `keep_on_close(false)`, remove the file on close.
boost::optional<temp_file>
mktemp( asio::io_service&, sys::error_code&
      , const fs::path& dir="."
      , const fs::path& model="tmp.%%%%-%%%%-%%%%-%%%%");

}} // namespaces
