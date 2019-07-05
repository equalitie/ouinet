#pragma once

#include <boost/asio/post.hpp>
#include "generic_stream.h"
#include "util/unique_function.h"

namespace ouinet {

/*
 * An IdleConnection wraps a GenericStream, and can be put in  "idle mode".
 * While in idle mode, the underlying stream is not allowed to become readable;
 * if it does, an error callback function is triggered.
 */
template<class Connection>
class IdleConnection {
    public:
    IdleConnection() {}

    IdleConnection(Connection&& connection):
        _data(std::make_unique<Data>())
    {
        _data->connection = std::move(connection);
        _data->was_destroyed = std::make_shared<bool>(false);
        _data->pending_idle_read = false;
        _data->read_queued = false;
    }

    ~IdleConnection()
    {
        if (_data) {
            *_data->was_destroyed = true;

            if (_data->read_callback) {
                asio::post(_data->connection.get_io_service(), [
                    handler = std::move(_data->read_callback)
                ] () mutable {
                    handler(asio::error::operation_aborted, 0);
                });
            }
        }
    }

    IdleConnection(const IdleConnection&) = delete;
    IdleConnection& operator=(const IdleConnection&) = delete;

    IdleConnection(IdleConnection&&) = default;
    IdleConnection& operator=(IdleConnection&&) = default;

    boost::asio::io_context::executor_type get_executor()
    {
        return _data->connection.get_executor();
    }

    boost::asio::io_context& get_io_service()
    {
        return _data->connection.get_io_service();
    }

    void close()
    {
        _data->connection.close();
    }

    bool is_open() const {
        if (!_data) return false;
        return _data->connection.is_open();
    }

    template<typename ConstBufferSequence, typename CompletionToken>
    BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken, void(sys::error_code, std::size_t))
    async_write_some(const ConstBufferSequence& buffers, CompletionToken&& token)
    {
        assert(_data);
        assert(_data->connection.has_implementation());
        return _data->connection.async_write_some(buffers, std::move(token));
    }

    template<typename MutableBufferSequence, typename CompletionToken>
    BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken, void(sys::error_code, std::size_t))
    async_read_some(const MutableBufferSequence& buffers, CompletionToken&& token)
    {
        assert(!_data->on_idle_read);

        bool empty = true;
        unsigned char* byte_buffer = nullptr;
        for (auto it = asio::buffer_sequence_begin(buffers); it != asio::buffer_sequence_end(buffers); ++it) {
            if (it->size() > 0) {
                empty = false;
                byte_buffer = (unsigned char*)it->data();
                break;
            }
        }

        if (empty) {
            // An operation with a zero-length buffer gets completed immediately.

            boost::asio::async_completion<
                CompletionToken,
                void(sys::error_code, std::size_t)
            > completion(token);

            asio::post(_data->connection.get_io_service(), [
                handler = std::move(completion.completion_handler)
            ] () mutable {
                handler(sys::error_code(), 0);
            });

            return completion.result.get();
        }

        if (_data->read_callback) {
            // Cannot support multiple simultaneous requests.

            boost::asio::async_completion<
                CompletionToken,
                void(sys::error_code, std::size_t)
            > completion(token);

            asio::post(_data->connection.get_io_service(), [
                handler = std::move(completion.completion_handler)
            ] () mutable {
                handler(asio::error::already_started, 0);
            });

            return completion.result.get();
        }

        /*
         * Three cases are possible:
         * - An idle read has completed before this read request got started, and we can
         *   fulfil this request immediately;
         * - An idle read is still pending, in which case we queue up this read request;
         * - No idle read is involved and we can pass along this read request directly.
         */

        if (_data->read_queued) {
            assert(!_data->pending_idle_read);
            _data->read_queued = false;

            size_t read = 0;
            if (!_data->queued_read_error) {
                *byte_buffer = _data->queued_read_buffer;
                read = 1;
            }

            boost::asio::async_completion<
                CompletionToken,
                void(sys::error_code, std::size_t)
            > completion(token);

            asio::post(_data->connection.get_io_service(), [
                handler = std::move(completion.completion_handler),
                read,
                ec = _data->queued_read_error
            ] () mutable {
                handler(ec, read);
            });

            return completion.result.get();
        }

        if (_data->pending_idle_read) {
            _data->read_buffer = byte_buffer;

            boost::asio::async_completion<
                CompletionToken,
                void(sys::error_code, std::size_t)
            > completion(token);

            _data->read_callback = [
                handler = std::move(completion.completion_handler)
            ] (sys::error_code ec, std::size_t read) mutable {
                handler(ec, read);
            };

            return completion.result.get();
        }

        return _data->connection.async_read_some(buffers, std::move(token));
    }

    void make_idle(std::function<void()> on_idle_read)
    {
        assert(!_data->read_callback);

        assert(!_data->on_idle_read);
        _data->on_idle_read = std::move(on_idle_read);

        if (_data->read_queued) {
            auto handler = std::move(_data->on_idle_read);
            handler();
            return;
        }

        if (_data->pending_idle_read) {
            return;
        }

        _data->pending_idle_read = true;
        _data->connection.async_read_some(
            boost::asio::mutable_buffer(&_data->queued_read_buffer, 1), [
                data = _data.get(),
                was_destroyed = _data->was_destroyed
            ] (sys::error_code ec, size_t read) {
                if (*was_destroyed) {
                    return;
                }

                /*
                 * Three cases are possible:
                 * - Connection is in idle mode, which makes this read unexpected;
                 * - Connection is not in idle mode, and there is a read request queued
                 *   to which we can forward the data;
                 * - Connection is not in idle mode and there is no read request queued.
                 *   Store the data for later and use it to complete the next request.
                 */

                assert(data->pending_idle_read);
                data->pending_idle_read = false;

                assert(!data->read_queued);

                if (data->on_idle_read) {
                    auto handler = std::move(data->on_idle_read);
                    handler();
                    return;
                }

                if (data->read_callback) {
                    if (read > 0) {
                        *data->read_buffer = data->queued_read_buffer;
                    }

                    asio::post(data->connection.get_io_service(), [
                        handler = std::move(data->read_callback),
                        ec,
                        read
                    ] () mutable {
                        handler(ec, read);
                    });

                    return;
                }

                data->queued_read_error = ec;
                data->read_queued = true;
            }
        );
    }

    void make_not_idle()
    {
        assert(_data->on_idle_read);
        _data->on_idle_read = nullptr;
    }

    private:
    struct Data {
        Connection connection;
        std::shared_ptr<bool> was_destroyed;

        /*
        * If set, the connection is idle, and read() returning is an error.
        */
        std::function<void()> on_idle_read;
        /*
        * True iff we are waiting on a read started while the connection was idle.
        */
        bool pending_idle_read;

        /*
        * Set when async_read_some() is invoked while the idle read call is still pending.
        */
        util::unique_function<void(sys::error_code, std::size_t)> read_callback;
        unsigned char* read_buffer;

        /*
        * Set when the idle read returns in anticipation of a future async_read_some() call.
        */
        bool read_queued;
        unsigned char queued_read_buffer;
        sys::error_code queued_read_error;
    };
    std::unique_ptr<Data> _data;
};

template<class StoredValue>
class ConnectionPool {
    public:
    class Connection;

    using Connections = std::list<Connection>;

    class Connection : public IdleConnection<GenericStream> {
        public:

        using IdleConnection<GenericStream>::IdleConnection;

        Connection(Connection&&) = default;
        Connection& operator=(Connection&&) = default;

        StoredValue& operator*()
        {
            return _value;
        }

        ~Connection()
        {
            if (!IdleConnection::is_open()) return;

            if (auto cs = _connections.lock()) {
                Connection c((IdleConnection<GenericStream>&&) *this);
                c._value = std::move(_value);
                push_back(*cs, std::move(c));
            }
        }

        private:

        Connection(IdleConnection<GenericStream> c)
            : IdleConnection<GenericStream>(std::move(c))
        {}

        private:
        friend class ConnectionPool;
        StoredValue _value;
        std::weak_ptr<Connections> _connections;
    };

    ConnectionPool()
        : _connections(std::make_shared<Connections>())
    {}

    Connection wrap(GenericStream connection)
    {
        auto c = Connection(std::move(connection));
        c._connections = _connections;
        return c;
    }

    void push_back(Connection connection)
    {
        connection._connections.reset();
        push_back(*_connections, std::move(connection));
    }

    Connection pop_front()
    {
        assert(!_connections->empty());

        Connection connection = std::move(_connections->front());
        _connections->pop_front();
        connection.make_not_idle();
        connection._connections = _connections;

        return connection;
    }

    bool empty() const
    {
        return _connections->empty();
    }

    private:
    static void push_back(Connections& connections, Connection connection)
    {
        connections.push_back(std::move(connection));

        typename Connections::iterator it = connections.end();
        --it;
        /*
         * Callback may be called during make_idle().
         * This is important, for the connection might be disqualified immediately.
         */
        it->make_idle([&connections, it] {
            it->close();
            connections.erase(it);
        });
    }

    private:
    std::shared_ptr<Connections> _connections;
};

} // namespace
