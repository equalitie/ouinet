#pragma once

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/append.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/udp.hpp>
#include <deque>
#include <limits>

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

    // Pushes a datagram into the buffer. This operation completes only when there is enough space
    // in the buffer to fit all of `buffers`. Otherwise it suspends until space is made available
    // with `async_pop` or `try_pop`.
    //
    // Yields the error code and the number of bytes pushed which is always equal to
    // `asio::buffer_size(buffers)` if the operation completes successfully or 0 if it fails.
    //
    // After the buffer has been closed, yields `asio::error::shut_down` immediately.
    template<typename ConstBufferSequence, typename Token>
    requires boost::asio::completion_token_for<Token, op_signature>
    auto async_push(
        const ConstBufferSequence& buffers,
        const boost::asio::ip::udp::endpoint& endpoint,
        Token&& token
    ) {
        return boost::asio::async_initiate<Token, op_signature>(
            [this, &buffers, &endpoint] (auto&& handler) {
                if (_closed) {
                    complete(std::move(handler), boost::asio::error::shut_down, 0);
                    return;
                }

                size_t n = try_push(buffers, endpoint);
                if (n > 0) {
                    complete(std::move(handler), boost::system::error_code(), n);
                    resume_pop();
                } else {
                    suspend_push(std::move(handler), buffers, endpoint);
                }
            },
            token
        );
    }

    // Attempt to push datagram into the buffer. Returns the number of bytes pushed which is either
    // equal to `asio::buffer_size(buffers)` if operation completes successfully or 0 if there is
    // not enough space in the buffer or if the buffer has been closed.
    template<typename ConstBufferSequence>
    size_t try_push(
        const ConstBufferSequence& buffers,
        const boost::asio::ip::udp::endpoint& endpoint
    ) {
        if (_closed || _data.capacity() - _data.size() < boost::asio::buffer_size(buffers)) {
            return 0;
        }

        size_t size = _data.write(buffers);
        _meta.push_back({ size, endpoint });

        resume_pop();

        return size;
    }

    // Pops a datagram from the buffer. This operation completes when there is at least one datagram
    // in the buffer. Otherwise it suspends until a datagram is pushed with `async_push` or
    // `try_push`.
    //
    // Yields the error code and the number of bytes popped which is equal to the datagram size or
    // to `asio::buffer_size(buffers)`, whichever is smaller or 0 if the operation fails. If
    // `buffers` is smaller than the datagram, the datagram is truncated and its remainder
    // discarded. To find the size of the datagram prior to calling this function, call `peek`.
    //
    // After the buffer has been closed, yields `asio::error::shut_down` immediately.
    template<typename MutableBufferSequence, typename Token>
    requires boost::asio::completion_token_for<Token, void(boost::system::error_code, size_t)>
    auto async_pop(
        const MutableBufferSequence& buffers,
        boost::asio::ip::udp::endpoint& endpoint,
        Token&& token
    ) {
        return boost::asio::async_initiate<Token, op_signature>(
            [this, &buffers, &endpoint] (auto&& handler) {
                if (_closed) {
                    complete(std::move(handler), boost::asio::error::shut_down, 0);
                    return;
                }

                size_t n = try_pop(buffers, endpoint);
                if (n > 0) {
                    complete(std::move(handler), boost::system::error_code(), n);
                    resume_push();
                } else {
                    suspend_pop(std::move(handler), buffers, endpoint);
                }
            },
            token
        );
    }

    // Attempt to pop datagram from the buffer. Returns the number of bytes popped which is always
    // equal to the datagram size or to `asio::buffer_size(buffers)`, whichever is smaller or 0 if
    // the buffer is empty. If `buffers` is smaller than the datagram, the datagram is truncated and
    // its remainder discarded. To find the size of the datagram prior to calling this function,
    // call `peek`.
    //
    // Note this function can be used also after the buffer has been closed to drain the remaining
    // datagrams.
    template<typename MutableBufferSequence>
    size_t try_pop(
        const MutableBufferSequence& buffers,
        boost::asio::ip::udp::endpoint& endpoint
    ) {
        if (_meta.empty()) {
            return 0;
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

    // Size of the next datagram to be popped. Can be 0 if the buffer is empty .
    size_t peek() const {
        if (_meta.empty()) {
            return 0;
        } else {
            return _meta.front().size;
        }
    }

    // Total number of bytes in the buffer, that is, the sum of the sizes of all the datagrams in
    // the buffer.
    size_t bytes() const {
        return _data.capacity() - _data.size();
    }

    // Whether there are no datagrams in the buffer.
    bool empty() const {
        return _data.size() == 0;
    }

private:

    struct Pending {
        boost::asio::any_completion_handler<op_signature> handler;
        std::function<size_t()> transfer;
    };

    template<typename Handler, typename Transfer>
    void suspend(std::optional<Pending>& pending, Handler&& handler, Transfer&& transfer) {
        if (pending) {
            complete(std::move(handler), boost::asio::error::already_started, 0);
            return;
        }

        auto slot = boost::asio::get_associated_cancellation_slot(handler);
        if (slot.is_connected()) {
            slot.assign([this, &pending] (boost::asio::cancellation_type type) {
                if (type != boost::asio::cancellation_type::none && pending) {
                    complete(pending, boost::asio::error::operation_aborted, 0);
                }
            });
        }

        pending.emplace(
            std::move(handler),
            std::move(transfer)
        );
    }

    template<typename Handler, typename ConstBufferSequence>
    void suspend_push(
        Handler&& handler,
        const ConstBufferSequence& buffers,
        const boost::asio::ip::udp::endpoint& endpoint
    ) {
        suspend(_pending_push, std::move(handler), [this, &buffers, &endpoint] {
            return try_push(buffers, endpoint);
        });
    }

    template<typename Handler, typename MutableBufferSequence>
    void suspend_pop(
        Handler&& handler,
        const MutableBufferSequence& buffers,
        boost::asio::ip::udp::endpoint& endpoint
    ) {
        suspend(_pending_pop, std::move(handler), [this, &buffers, &endpoint] {
           return try_pop(buffers, endpoint);
        });
    }

    void resume(std::optional<Pending>& pending) {
        if (!pending) {
            return;
        }

        size_t n = pending->transfer();
        if (n > 0) {
            complete(pending, boost::system::error_code(), n);
        }
    }

    void resume_push() {
        resume(_pending_push);
    }

    void resume_pop() {
        resume(_pending_pop);
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
