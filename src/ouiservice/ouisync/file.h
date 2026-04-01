#pragma once

#include <ouisync.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/detached.hpp>
#include "../../namespaces.h"

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
    typedef asio::any_io_executor executor_type;

    static OuisyncFile init(ouisync::File, asio::yield_context);
    static OuisyncFile empty(asio::any_io_executor);

    OuisyncFile() = default;

    OuisyncFile(OuisyncFile const&) = delete;
    OuisyncFile(OuisyncFile &&) = default;

    const executor_type& get_executor() const { return _exec; }

    template<class Buffer, class Token>
    auto async_read_some(const Buffer& buffer, Token token) {
        return boost::asio::async_initiate<Token, void(sys::error_code, size_t)>(
            [ state = _state,
              buffer = std::move(buffer)
            ] (auto handler) mutable {
                // Reference
                // https://www.boost.org/doc/libs/1_89_0/doc/html/boost_asio/reference/AsyncReadStream.html
                if (state->closed) {
                    return handler(asio::error::operation_aborted, 0);
                }

                size_t to_read = asio::buffer_size(buffer);
                
                if (to_read == 0) {
                    return handler(sys::error_code(), 0);
                }
                
                if (!state->_inner) {
                    return handler(asio::error::not_connected, 0);
                }

                if (state->_offset == state->_file_size) {
                    return handler(asio::error::eof, 0);
                }
                
                if (state->_offset + to_read > state->_file_size) {
                    to_read = state->_file_size - state->_offset;
                }
                
                auto s = state.get(); // because we're moving below

                s->_inner->read(s->_offset, to_read,
                    [ state = std::move(state),
                      handler = std::move(handler),
                      buffer = std::move(buffer)
                    ]
                    (sys::error_code ec, std::vector<uint8_t> data) mutable {
                        if (state->closed) {
                            ec = asio::error::operation_aborted;
                        }

                        if (ec) {
                            return handler(ec, data.size());
                        }

                        auto copied = asio::buffer_copy(buffer, asio::buffer(data));
                        state->_offset += copied;

                        handler(ec, copied);
                    });
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
    OuisyncFile(executor_type exec, std::shared_ptr<State> state) :
        _exec(std::move(exec)),
        _state(std::move(state))
    {}

private:
    executor_type _exec;
    std::shared_ptr<State> _state;
};

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
