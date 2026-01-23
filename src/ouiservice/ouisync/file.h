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
private:
    struct State {
        size_t _file_size;
        size_t _offset;
        bool closed;
        // `!_inner && !closed` means an empty file
        std::unique_ptr<ouisync::File> _inner;

        template<class Buffer>
        size_t async_read_some_y(Buffer& buffer, asio::yield_context);

        void close(asio::yield_context yield) {
            if (closed) return;
            closed = true;
            if (_inner) {
                _inner->close(yield);
                _inner = nullptr;
            }
        }
    };

public:
    typedef util::AsioExecutor executor_type;

    static OuisyncFile init(ouisync::File, asio::yield_context);
    static OuisyncFile empty(asio::any_io_executor);

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
                if (!is_open()) {
                    handler(asio::error::bad_descriptor, 0);
                    return;
                }
                // TODO: We need to implement the async result functionality in
                // ouisync/cpp bindings to get rid of this spawn
                asio::spawn(_exec, [state = _state, buffer, handler = std::move(handler)](auto yield) mutable {
                    sys::error_code ec;
                    if (!state->_inner || state->_file_size == 0) {
                        handler(ec, 0);
                        return;
                    }
                    auto size = state->async_read_some_y(buffer, yield[ec]);
                    handler(ec, size);
                },
                asio::detached);
            },
            token
        );
    }

    size_t size(sys::error_code& ec) const {
        if (!_state) {
            ec = asio::error::bad_descriptor;
            return 0;
        }
        return _state->_file_size;
    }

    void fseek(size_t pos, sys::error_code ec) {
        if (!_state) {
            ec = asio::error::bad_descriptor;
            return;
        }
        _state->_offset = pos;
    }

    void close() {
        if (!_state) return;
        if (_state->closed) { _state = nullptr; return; }
        asio::spawn(_exec, [state = std::move(_state)] (auto yield) {
                state->close(yield);
            },
            asio::detached);
    }

    void close(asio::yield_context yield) {
        if (!_state) return;
        if (_state->closed) { _state = nullptr; return; }
        auto state = std::move(_state);
        state->close(yield);
    }

    bool is_open() const {
        return _state != nullptr && _state->closed == false;
    }

    ~OuisyncFile();

private:
    OuisyncFile(util::AsioExecutor exec, std::shared_ptr<State> state) :
        _exec(std::move(exec)),
        _state(std::move(state))
    {}

private:
    util::AsioExecutor _exec;
    std::shared_ptr<State> _state;
};

// Reference
// https://www.boost.org/doc/libs/1_89_0/doc/html/boost_asio/reference/AsyncReadStream.html
template<class Buffer>
size_t OuisyncFile::State::async_read_some_y(Buffer& buffer, boost::asio::yield_context yield) {
    size_t to_read = asio::buffer_size(buffer);

    if (to_read == 0) {
        return 0;
    }

    if (!_inner) {
        close(yield);
        return or_throw(yield, asio::error::not_connected, 0);
    }

    if (_offset == _file_size) {
        close(yield);
        return or_throw(yield, asio::error::eof, 0);
    }

    if (_offset + to_read > _file_size) {
        to_read = _file_size - _offset;
    }

    sys::error_code ec;
    std::vector<uint8_t> data = _inner->read(_offset, to_read, yield[ec]);

    if (closed) ec = asio::error::operation_aborted;

    if (ec) {
        close(yield);
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
    return file.size(ec);
}

inline
void fseek(ouisync_service::OuisyncFile& file, size_t pos, sys::error_code& ec) {
    file.fseek(pos, ec);
}

} // namespace ouinet::util::file_io
