#pragma once

#include <ouisync.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/detached.hpp> // TODO: this won't be needed once ouisync supports async result
#include "../../util/executor.h"
#include "../../namespaces.h"
#include "../../or_throw.h"
#include <iostream>

namespace ouinet::ouisync_service {

// Wrapper over ouisync::File with Asio's `AsyncReadStream` interface so it can
// be used with asio functions such as `async_read`.
class OuisyncFile {
public:
    typedef util::AsioExecutor executor_type;

    static OuisyncFile init(ouisync::File, asio::yield_context);

    OuisyncFile() = default;

    OuisyncFile(OuisyncFile const&) = delete;
    OuisyncFile(OuisyncFile &&) = default;

    const util::AsioExecutor& get_executor() const { return _exec; }

    template<class Buffer>
    size_t async_read_some_y(Buffer&, asio::yield_context);

    template<class Buffer, class Token>
    auto async_read_some(const Buffer& buffer, Token token) {
        return asio::async_initiate<
          Token,
          void (boost::system::error_code, std::size_t)>(
            [&buffer, this](auto handler) {
                // TODO: We need to implement the async result functionality in
                // ouisync/cpp bindings to get rid of this spawn
                asio::spawn(_exec, [this, buffer, handler = std::move(handler)](auto yield) mutable {
                    sys::error_code ec;
                    auto size = async_read_some_y(buffer, yield[ec]);
                    handler(ec, size);
                },
                asio::detached);
            },
            token
        );
    }

    size_t size() const {
        return _file_size;
    }

    void fseek(size_t pos) {
        _offset = pos;
    }

    void close() {
        _inner = nullptr;
    }

    bool is_open() const {
        return _inner != nullptr;
    }

private:
    OuisyncFile(util::AsioExecutor exec, size_t size, ouisync::File file) :
        _file_size(size),
        _offset(0),
        _exec(std::move(exec)),
        _inner(std::make_unique<ouisync::File>(std::move(file)))
    {}

private:
    size_t _file_size;
    size_t _offset;
    util::AsioExecutor _exec;
    std::unique_ptr<ouisync::File> _inner;
};

// Reference
// https://www.boost.org/doc/libs/1_89_0/doc/html/boost_asio/reference/AsyncReadStream.html
template<class Buffer>
size_t OuisyncFile::async_read_some_y(Buffer& buffer, boost::asio::yield_context yield) {
    size_t to_read = asio::buffer_size(buffer);

    if (!_inner) {
        return or_throw(yield, asio::error::not_connected, 0);
    }
    
    if (to_read == 0) {
        close();
        return or_throw(yield, {}, 0);
    }

    if (!_inner) {
        close();
        return or_throw(yield, asio::error::not_connected, 0);
    }

    if (_offset == _file_size) {
        close();
        return or_throw(yield, asio::error::eof, 0);
    }

    if (_offset + to_read > _file_size) {
        to_read = _file_size - _offset;
    }

    sys::error_code ec;
    std::vector<uint8_t> data = _inner->read(_offset, to_read, yield[ec]);

    if (ec) {
        close();
        return or_throw(yield, ec, 0);
    }

    auto copied = asio::buffer_copy(buffer, asio::buffer(data));
    _offset += copied;

    return copied;
}

} // namespace ouinet::ouisync_service

namespace ouinet::util::file_io {

inline
size_t file_size(ouisync_service::OuisyncFile& file, sys::error_code& ec) {
    return file.size();
}

inline
void fseek(ouisync_service::OuisyncFile& file, size_t pos, sys::error_code&) {
    file.fseek(pos);
}

} // namespace ouinet::util::file_io
