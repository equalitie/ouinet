#pragma once

#include <boost/intrusive/list.hpp>
#include <boost/optional.hpp>
#include "generic_stream.h"
#include "util/condition_variable.h"
#include "util/signal.h"
#include "or_throw.h"
#include "defer.h"

namespace ouinet {

template<class Aux = boost::none_t>
class ConnectionPool {
    using Request  = http::request<http::string_body>;
    using Response = http::response<http::dynamic_body>;

    using ListHook = boost::intrusive::list_base_hook
        <boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

    template<class T>
    using List = boost::intrusive::list
        <T, boost::intrusive::constant_time_size<false>>;

    public:
    struct Connection : public ListHook {
        public:
        Connection(GenericStream stream, Aux aux)
            : aux(std::move(aux))
            , _stream(std::move(stream))
            , _cv(_stream.get_io_service())
            , _was_destroyed(std::make_shared<bool>(false))
        {
            asio::spawn(_stream.get_io_service(),
                [&, wd = _was_destroyed] (asio::yield_context yield) {
                    beast::flat_buffer buffer;
                    sys::error_code ec;

                    while (!ec) {
                        Response res;
                        http::async_read(_stream, buffer, res, yield[ec]);

                        if (*wd) return;

                        if (!_is_requesting && !ec) {
                            std::cerr << "Remote sent a response without "
                                      << " us requesting anything: " << std::endl
                                      << res;
                            assert(0);
                        }
                        if (!_is_requesting && !ec) break;

                        _res = std::move(res);
                        _cv.notify(ec);
                    }

                    _stream.close();
                    _self.reset();
                });
        }

        Connection(const Connection&) = delete;
        Connection(Connection&&) = delete;

        Response request(Request rq, Cancel& cancel, asio::yield_context yield)
        {
            assert(!_is_requesting);
            assert(!_res);

            if (cancel.call_count())
                return or_throw<Response>(yield, asio::error::operation_aborted);

            _is_requesting = true;
            auto on_exit = defer([&] { _is_requesting = false; });

            if (!_stream.has_implementation()) {
                return or_throw<Response>(yield, asio::error::bad_descriptor);
            }

            auto wd = _was_destroyed;

            auto cancel_slot = cancel.connect([&] { _stream.close(); });

            sys::error_code ec;
            http::async_write(_stream, rq, yield[ec]);

            if (!ec && (*wd || cancel.call_count()))
                    ec = asio::error::operation_aborted;

            if (ec) return or_throw<Response>(yield, ec);

            if (!_res) _cv.wait(yield[ec]);

            if (!ec && (*wd || cancel.call_count()))
                ec = asio::error::operation_aborted;

            if (ec) return or_throw<Response>(yield, ec);

            auto ret = std::move(*_res);
            _res = boost::none;
            return ret;
        }

        ~Connection()
        {
            *_was_destroyed = true;
        }

        void close() {
            _stream.close();
        }

        void* id() const { return _stream.id(); }

        // Auxilliary data per stream.
        Aux aux;

        private:
        friend class ConnectionPool;
        bool _is_requesting = false;
        GenericStream _stream;
        ConditionVariable _cv;
        boost::optional<Response> _res;
        std::shared_ptr<bool> _was_destroyed;

        // Keep `this` from destruction while in the pool.
        std::unique_ptr<Connection> _self;
    };

    public:
    void push_back(std::unique_ptr<Connection> c)
    {
        assert(c);
        _connections.push_back(*c);
        c->_self = std::move(c);
    }

    std::unique_ptr<Connection> pop_front()
    {
        if (_connections.empty()) return nullptr;
        auto& front = _connections.front();
        _connections.pop_front();
        return std::move(front._self);
    }

    bool empty() const { return _connections.empty(); }

    private:
    List<Connection> _connections;
};

} // namespace
