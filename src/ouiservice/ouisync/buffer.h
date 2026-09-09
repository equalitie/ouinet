#pragma once

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/append.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/system/detail/error_code.hpp>
#include <deque>
#include <limits>
#include <optional>

namespace ouinet {
namespace ouisync_service {
namespace detail {

// Fixed capacity ring buffer which does not overwrite data.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : _data(capacity, 0)
    {
        assert(capacity > 0 && "capacity must be non-zero");
    }

    RingBuffer() = delete;
    RingBuffer(const RingBuffer&) = default;
    RingBuffer(RingBuffer&&) = default;

    RingBuffer& operator = (const RingBuffer&) = default;
    RingBuffer& operator = (RingBuffer&) = default;

    // Write data from `buffers` to this ring buffer. Returns the number of bytes actually written
    // (which could be less than the size of `buffers` if there is not enough available space in the
    // ring buffer to fit it all).
    template<typename ConstBufferSequence>
    size_t write(const ConstBufferSequence& buffers) noexcept {
        auto n = boost::asio::buffer_copy(writable(), buffers);
        _size += n;

        return n;
    }

    // Read data from this ring buffer to `buffers`. Returns the number of bytes read (which can
    // be less than the size of `buffers` if there is not enough data in the ring buffer to fill
    // it completely).
    template<typename MutableBufferSequence>
    size_t read(
        const MutableBufferSequence& buffers,
        size_t max = std::numeric_limits<size_t>::max()
    ) noexcept {
        auto n = boost::asio::buffer_copy(buffers, readable(), max);
        _size -= n;
        _begin = (_begin + n) % capacity();

        return n;
    }

    // Discard `count` bytes from the buffer. This is equivalent to calling `read` with `buffers` of
    // size `count` and then discarding the buffers, but more efficient as it doesn't require
    // allocating the buffers. Returns the number of bytes actually discarded.
    size_t discard(size_t count) noexcept {
        auto n = std::min(count, _size);
        _size -= n;
        _begin = (_begin + n) % capacity();

        return n;
    }

    // Amount of data in the buffer.
    inline size_t size() const noexcept {
        return _size;
    }

    // Max amount of data this buffer can hold.
    inline size_t capacity() const noexcept {
        return _data.size();
    }

    // Amount of available space in this buffer (capacity - size)
    inline size_t space() const noexcept {
        return capacity() - size();
    }

private:

    std::array<boost::asio::const_buffer, 2> readable() const noexcept {
        size_t end = (_begin + _size) % capacity();

        if (_begin <= end) {
            return {
                boost::asio::const_buffer(_data.data() + _begin, end - _begin),
                boost::asio::const_buffer()
            };
        } else {
            return {
                boost::asio::const_buffer(_data.data() + _begin, capacity() - _begin),
                boost::asio::const_buffer(_data.data(), end)
            };
        }
    }

    std::array<boost::asio::mutable_buffer, 2> writable() noexcept {
        size_t end = (_begin + _size) % capacity();

        if (_begin <= end) {
            return {
                boost::asio::mutable_buffer(_data.data() + end, capacity() - end),
                boost::asio::mutable_buffer(_data.data(), _begin)
            };
        } else {
            return {
                boost::asio::mutable_buffer(_data.data() + end, _begin - end),
                boost::asio::mutable_buffer()
            };
        }
    }

private:
    std::vector<uint8_t> _data;
    size_t _begin = 0; // index of the first readable byte
    size_t _size = 0; // number o bytes written to the buffer
};

struct DatagramMetadata {
    size_t size;
    boost::asio::ip::udp::endpoint endpoint;
};

// Asynchronous queue for buffering UDP datagrams.
//
// Note at most one push and one pop operation at a time is supported.
class AsyncDatagramBuffer {
public:
    using op_signature = void(boost::system::error_code, size_t);
    using executor_type = boost::asio::any_io_executor;

public:
    explicit AsyncDatagramBuffer(boost::asio::any_io_executor ex, size_t capacity) :
        _ex(std::move(ex)),
        _data(capacity)
    {}

    AsyncDatagramBuffer() = delete;
    AsyncDatagramBuffer(const AsyncDatagramBuffer&) = delete;
    AsyncDatagramBuffer(AsyncDatagramBuffer&&) = delete;

    AsyncDatagramBuffer& operator = (const AsyncDatagramBuffer&) = delete;
    AsyncDatagramBuffer& operator = (AsyncDatagramBuffer&) = delete;

    ~AsyncDatagramBuffer() {
        cancel();
    }

    // Returns the executor associated with this buffer.
    const executor_type& get_executor() {
        return _ex;
    }

    // Cancels all pending operations (push and/or pop). The buffer can still be used normally
    // afterwards.
    void cancel() {
        complete(_pending_push, boost::asio::error::operation_aborted, 0);
        complete(_pending_pop, boost::asio::error::operation_aborted, 0);
    }

    // Closes the buffer. Cancels all pending operations. All subsequent async_push and async_pop
    // calls immediately yield `asio::error::shut_down`. Datagrams remaining in the buffer can be
    // drained with `try_pop`.
    void close() {
        cancel();
        _closed = true;
    }

    // Waits until the buffer has enough space for a datagram of the given size. Completes with the
    // size of the available space in the buffer (which can be greater than `size`).
    template<typename Token>
    auto async_wait_push(size_t size, Token&& token) {
        return boost::asio::async_initiate<Token, op_signature>(
          [this, size] (auto&& handler) {
              if (_closed) {
                  complete(std::move(handler), boost::asio::error::shut_down, 0);
                  return;
              }

              if (_data.space() >= size) {
                  complete(std::move(handler), boost::system::error_code(), _data.space());
              } else {
                  suspend_push(std::move(handler), size);
              }
          },
          token
        );
    }

    // Attempt to push datagram into the buffer. Returns the number of bytes pushed (which is always
    // equal to `asio::buffer_size(buffers)`) or `nullopt` is the buffer has insufficient available
    // space.
    template<typename ConstBufferSequence>
    std::optional<size_t> try_push(
        const ConstBufferSequence& buffers,
        const boost::asio::ip::udp::endpoint& endpoint
    ) {
        if (_closed || _data.space() < boost::asio::buffer_size(buffers)) {
            return std::nullopt;
        }

        size_t size = _data.write(buffers);
        _meta.push_back({ size, endpoint });

        resume_pop();

        return size;
    }

    // Pushes the datagram into the buffer, asynchronously waiting for available space if necessary.
    // Completes with the number of bytes actually pushed which is equal to the
    // `asio::buffer_size(buffers)` if the operation completed successfully or 0 otherwise.
    template<typename ConstBufferSequence, typename Token>
    requires boost::asio::completion_token_for<Token, op_signature>
    auto async_push(
        const ConstBufferSequence& buffers,
        const boost::asio::ip::udp::endpoint& endpoint,
        Token&& token
    ) {
        return boost::asio::async_compose<Token, op_signature>(
            [this, buffers, &endpoint]
            (auto& self, boost::system::error_code ec = {}, size_t size = 0, bool done = false) {
                if (done || ec) {
                    self.complete(ec, size);
                    return;
                }

                auto n = try_push(buffers, endpoint);
                if (n) {
                    boost::asio::post(
                        _ex,
                        boost::asio::append(std::move(self), boost::system::error_code(), *n, true)
                    );
                    return;
                }

                self.reset_cancellation_state(boost::asio::enable_total_cancellation());
                async_wait_push(boost::asio::buffer_size(buffers), std::move(self));
            },
            token,
            _ex
        );
    }

    // Waits until the buffer has at least one datagram in it. Completes with the size of the first
    // available datagram.
    template<typename Token>
    auto async_wait_pop(Token&& token) {
        return boost::asio::async_initiate<Token, op_signature>(
            [this] (auto&& handler) {
                if (_closed) {
                    complete(std::move(handler), boost::asio::error::shut_down, 0);
                    return;
                }

                if (!_meta.empty()) {
                    complete(std::move(handler), boost::system::error_code(), _meta.front().size);
                } else {
                    suspend_pop(std::move(handler));
                }
            },
            token
        );
    }

    // Attempt to pop datagram from the buffer. Returns the number of bytes popped (which can be
    // zero for payloadless datagrams) or `nullopt` if the buffer is empty.
    //
    // Note: if `buffers` is not sufficiently large to fit the whole datagram, the datagram is
    // truncated and its remainder discarded. To prevent this, call `peek` to find the actual size
    // of the next datagram.
    //
    // Note this function can be used also after the buffer has been closed to drain the remaining
    // datagrams.
    template<typename MutableBufferSequence>
    std::optional<size_t> try_pop(
        const MutableBufferSequence& buffers,
        boost::asio::ip::udp::endpoint& endpoint
    ) {
        if (_meta.empty()) {
            return std::nullopt;
        }

        size_t size = _meta.front().size;
        endpoint = _meta.front().endpoint;
        _meta.pop_front();

        size_t n = _data.read(buffers, size);
        if (n < size) {
            _data.discard(size - n);
        }

        resume_push();

        return n;
    }

    // Pops a datagram from the buffer, asynchronously waiting for one to become available if
    // necessary. Completes with the size of the popped datagram.
    //
    // Note: if `buffers` is not sufficiently large to fit the whole datagram, the datagram is
    // truncated and its remainder discarded. To prevent this, call `async_wait_pop` which yields
    // the size of the next datagram, then allocate sufficiently large buffer and pass it to
    // `try_pop`.
    template<typename MutableBufferSequence, typename Token>
    requires boost::asio::completion_token_for<Token, op_signature>
    auto async_pop(
        const MutableBufferSequence& buffers,
        boost::asio::ip::udp::endpoint& endpoint,
        Token&& token
    ) {
        return boost::asio::async_compose<Token, op_signature>(
            [this, buffers, &endpoint]
            (auto& self, boost::system::error_code ec = {}, size_t size = 0, bool done = false) {
                if (done || ec) {
                    self.complete(ec, size);
                    return;
                }

                auto n = try_pop(buffers, endpoint);
                if (n) {
                    boost::asio::post(
                        _ex,
                        boost::asio::append(std::move(self), boost::system::error_code(), *n, true)
                    );
                    return;
                }

                // Default cancellation filter is `terminal` but we want `total`.
                self.reset_cancellation_state(boost::asio::enable_total_cancellation());

                async_wait_pop(std::move(self));
            },
            token,
            _ex
        );
    }

    // Size of the next datagram to be popped or `nullopt` if the buffer is empty .
    std::optional<size_t> peek() const {
        if (_meta.empty()) {
            return std::nullopt;
        } else {
            return _meta.front().size;
        }
    }

    // Total number of bytes in the buffer, that is, the sum of the sizes of all the datagrams in
    // the buffer.
    size_t bytes() const {
        return _data.size();
    }

    // Whether there are no datagrams in the buffer.
    bool empty() const {
        return _meta.empty();
    }

private:

    struct Pending {
        boost::asio::any_completion_handler<op_signature> handler;
        size_t size; // used only for push
    };

    template<typename Handler>
    void suspend(std::optional<Pending>& pending, Handler&& handler, size_t size) {
        if (pending) {
            complete(std::move(handler), boost::asio::error::already_started, 0);
            return;
        }

        pending.emplace(std::move(handler), size);

        auto slot = boost::asio::get_associated_cancellation_slot(pending->handler);
        if (slot.is_connected()) {
            slot.assign([this, &pending] (boost::asio::cancellation_type type) {
                if (type != boost::asio::cancellation_type::none && pending) {
                    complete(pending, boost::asio::error::operation_aborted, 0);
                }
            });
        }
    }

    template<typename Handler>
    void suspend_push(Handler&& handler, size_t size) {
        suspend(_pending_push, std::move(handler), size);
    }

    template<typename Handler>
    void suspend_pop(Handler&& handler) {
        suspend(_pending_pop, std::move(handler), 0);
    }

    void resume_push() {
        if (!_pending_push) {
            return;
        }

        if (_data.space() >= _pending_push->size) {
            complete(_pending_push, boost::system::error_code(), _data.space());
        }
    }

    void resume_pop() {
        if (!_pending_pop) {
            return;
        }

        if (!_meta.empty()) {
            complete(_pending_pop, boost::system::error_code(), _meta.front().size);
        }
    }

    template<typename Handler>
    void complete(Handler&& handler, boost::system::error_code ec, size_t size) {
        boost::asio::post(_ex, boost::asio::append(std::move(handler), ec, size));
    }

    void complete(std::optional<Pending>& pending, boost::system::error_code ec, size_t size) {
        if (pending) {
            if (pending->handler) {
                complete(std::move(pending->handler), ec, size);
            }
            pending.reset();
        }
    }

private:
    boost::asio::any_io_executor _ex;
    // Payloads of all enqueued datagrams stored in one continuous buffer, one after another. The
    // boundaries between the datagrams are determined by the metadata in the `_meta` queue.
    RingBuffer _data;
    std::deque<DatagramMetadata> _meta;
    bool _closed = false;
    std::optional<Pending> _pending_push;
    std::optional<Pending> _pending_pop;
};

}}} // namespace ouinet::ouisync_service::detail
