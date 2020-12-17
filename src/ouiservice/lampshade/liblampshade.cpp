#include <lampshade_bindings.h>

#include <boost/asio/error.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/optional.hpp>
#include <boost/system/error_code.hpp>
#include <experimental/tuple>

#include "liblampshade.h"
#include "../../logger.h"
#include "../../or_throw.h"
#include "../../util/str.h"

namespace ouinet {
namespace lampshade {

/*
 * AsyncCall is a structure encapsulating an asynchronous call to the golang
 * API, the body is which is executed in the golang thread, and which can
 * clean up after itself if the asynchronous call is cancelled and then
 * destroyed in the asio thread.
 */

template<class... As>
struct AsyncCall : public detail::Cancellable {
    boost::asio::executor _ex;
    std::function<void(boost::system::error_code, As&&...)> _callback;
    std::function<void()> _cancel_function;
    boost::optional<Signal<void()>::Connection> _cancel_connection;
    boost::optional<uint64_t> _cancellation_id;
    boost::asio::io_service::work _work;

    AsyncCall(
        const boost::asio::executor& ex,
        boost::intrusive::list<detail::Cancellable, boost::intrusive::constant_time_size<false>>* call_list,
        boost::optional<uint64_t> cancellation_id,
        Signal<void()>& cancel_signal,
        std::function<void(boost::system::error_code, As&&...)> callback
    ):
        _ex(ex),
        _cancellation_id(cancellation_id),
        _work(boost::asio::io_service::work(_ex))
    {
        if (call_list) {
            call_list->push_back(*this);
        }

        _callback = [
            this,
            callback = std::move(callback)
        ] (boost::system::error_code ec, As... args) {
            _cancel_function = []{};
            // We need to unlink here, othersize the callback could invoke the
            // destructor, which would in turn call `cancel` and expect that it
            // gets unlinked. But we just set the `_cancel_function` to do nothing
            // above, so the destructor ends up in an infinite loop.
            unlink();
            std::experimental::apply(callback, std::make_tuple(ec, std::move(args)...));
        };

        _cancel_function = [this] {
            unlink();
            if (_cancellation_id) {
                go_lampshade_cancellation_cancel(*_cancellation_id);
            }
            asio::post(_ex, [this, callback = std::move(_callback)] {
                std::tuple<boost::system::error_code, As...> args;
                std::get<0>(args) = boost::asio::error::operation_aborted;
                std::experimental::apply(callback, std::move(args));
            });
            _cancel_function = []{};
        };

        _cancel_connection = cancel_signal.connect([this] {
            _cancel_function();
        });

        /*
         * Exactly one of _callback and *_cancel_function is ever called, in the asio thread.
         */
    }

    /*
     * This function is always called, in a go thread. If the AsyncCall was
     * cancelled, self->_callback is empty.
     */
    static void call(void* arg, int error, As... args) {
        auto self = reinterpret_cast<AsyncCall*>(arg);

        if (self->_cancellation_id) {
            go_lampshade_cancellation_free(*self->_cancellation_id);
        }

        boost::system::error_code ec;
        if (error) {
            ec = boost::system::errc::make_error_code(boost::system::errc::no_message);
        }
        asio::post(self->_ex, [
            self,
            full_args = std::make_tuple(ec, std::move(args)...)
        ] {
            std::unique_ptr<AsyncCall> self_(self);
            if (self_->_callback) {
                std::experimental::apply(self_->_callback,
                    std::tuple<boost::system::error_code, As...>(std::move(full_args))
                );
            }
        });
    }

    void cancel() override {
        _cancel_function();
    }
};


template<class A> struct callback_function;

template<> struct callback_function<void> {
    static void callback(void* arg, int error) {
        AsyncCall<>::call(arg, error);
    }
};

template<> struct callback_function<uint64_t> {
    static void callback(void* arg, int error, uint64_t value) {
        AsyncCall<uint64_t>::call(arg, error, value);
    }
};

template<class A> struct YieldHandler {
    typedef typename asio::handler_type<asio::yield_context, void(sys::error_code, A)>::type Handler;
    typedef AsyncCall<A> Call;
};
template<> struct YieldHandler<void> {
    typedef typename asio::handler_type<asio::yield_context, void(sys::error_code)>::type Handler;
    typedef AsyncCall<> Call;
};

template<class Output, class F, class... As>
Output call_lampshade_cancellable(
    const boost::asio::executor& ex,
    asio::yield_context yield,
    boost::intrusive::list<detail::Cancellable, boost::intrusive::constant_time_size<false>>* operation_list,
    Signal<void()>& cancel,
    F lampshade_function,
    As... args
) {
    uint64_t cancellation_id = go_lampshade_cancellation_allocate();

    typename YieldHandler<Output>::Handler handler(std::forward<asio::yield_context>(yield));
    asio::async_result<typename YieldHandler<Output>::Handler> result(handler);

    lampshade_function(
        args...,
        cancellation_id,
        (void*) &callback_function<Output>::callback,
        (void*) (new typename YieldHandler<Output>::Call{ ex, operation_list, cancellation_id, cancel, handler })
    );

    return result.get();
}

template<class Output, class F, class... As>
Output call_lampshade_uncancellable(
    const boost::asio::executor& ex,
    asio::yield_context yield,
    boost::intrusive::list<detail::Cancellable, boost::intrusive::constant_time_size<false>>* operation_list,
    Signal<void()>& cancel,
    F lampshade_function,
    As... args
) {
    typename YieldHandler<Output>::Handler handler(std::forward<asio::yield_context>(yield));
    asio::async_result<typename YieldHandler<Output>::Handler> result(handler);

    lampshade_function(
        args...,
        (void*) &callback_function<Output>::callback,
        (void*) (new typename YieldHandler<Output>::Call{ ex, operation_list, boost::none, cancel, handler })
    );

    return result.get();
}



void empty_lampshade_callback_void(void* arg, int error)
{
}



class LampshadeStream
{
    public:
    LampshadeStream(const asio::executor& ex, uint64_t connection_id):
        _ex(ex),
        _connection_id(connection_id),
        _closed(false)
    {}

    LampshadeStream(LampshadeStream&& other):
        _ex(std::move(other._ex)),
        _pending_operations(std::move(other._pending_operations)),
        _connection_id(other._connection_id),
        _closed(other._closed)
    {
        other._connection_id.reset();
    }

    ~LampshadeStream()
    {
        while (!_pending_operations.empty()) {
            auto& e = _pending_operations.front();
            e.cancel();
            /*
             * The handle will unlink itself in cancel(),
             * so there is no need to pop_front().
             */
        }

        if (_connection_id) {
            if (!_closed) {
                close();
            }
            go_lampshade_connection_free(*_connection_id);
        }
    }

    asio::executor get_executor()
    {
        return _ex;
    }

    void async_read_some(std::vector<asio::mutable_buffer>& buffers, std::function<void(sys::error_code, size_t)>&& callback)
    {
        asio::mutable_buffer buffer;
        bool found = false;
        for (size_t i = 0; i < buffers.size(); i++) {
            if (buffers[i].size() != 0) {
                found = true;
                buffer = buffers[i];
                break;
            }
        }
        if (!found) {
            asio::post(_ex, [this, callback = std::move(callback)] {
                callback(sys::error_code(), 0);
            });
        }

        asio::spawn(_ex, [
            this,
            buffer,
            callback = std::move(callback)
        ] (asio::yield_context yield) {
            sys::error_code ec;
            Signal<void()> cancel_signal;

            uint64_t read = call_lampshade_uncancellable<uint64_t>(
                _ex,
                yield[ec],
                &_pending_operations,
                cancel_signal,

                go_lampshade_connection_receive,
                *_connection_id,
                buffer.data(),
                buffer.size()
            );

            asio::post(_ex, [this, ec, read, callback = std::move(callback)] {
                callback(ec, read);
            });
        });
    }

    void async_write_some(const std::vector<asio::const_buffer>& buffers, std::function<void(sys::error_code, size_t)>&& callback)
    {
        asio::const_buffer buffer;
        bool found = false;
        for (size_t i = 0; i < buffers.size(); i++) {
            if (buffers[i].size() != 0) {
                found = true;
                buffer = buffers[i];
                break;
            }
        }
        if (!found) {
            asio::post(_ex, [this, callback = std::move(callback)] {
                callback(sys::error_code(), 0);
            });
        }

        asio::spawn(_ex, [
            this,
            buffer,
            callback = std::move(callback)
        ] (asio::yield_context yield) {
            sys::error_code ec;
            Signal<void()> cancel_signal;

            uint64_t read = call_lampshade_uncancellable<uint64_t>(
                _ex,
                yield[ec],
                &_pending_operations,
                cancel_signal,

                go_lampshade_connection_send,
                *_connection_id,
                (void*) buffer.data(),
                buffer.size()
            );

            asio::post(_ex, [this, ec, read, callback = std::move(callback)] {
                callback(ec, read);
            });
        });
    }

    void close()
    {
        go_lampshade_connection_close(*_connection_id, (void*) empty_lampshade_callback_void, nullptr);
    }

    bool is_open() const {
        if (_closed) return false;
        return bool(_connection_id);
    }

    protected:
    asio::executor _ex;
    boost::intrusive::list<detail::Cancellable, boost::intrusive::constant_time_size<false>> _pending_operations;
    boost::optional<uint64_t> _connection_id;
    bool _closed;
};



Dialer::Dialer(const asio::executor& ex):
    _ex(ex)
{
    _dialer_id = go_lampshade_dialer_allocate();
}

Dialer::~Dialer()
{
    while (!_pending_operations.empty()) {
        auto& e = _pending_operations.front();
        e.cancel();
        /*
         * The handle will unlink itself in cancel(),
         * so there is no need to pop_front().
         */
    }

    go_lampshade_dialer_free(_dialer_id);
}

void Dialer::init(asio::ip::tcp::endpoint endpoint, std::string public_key_der, asio::yield_context yield)
{
    std::string endpoint_string = util::str(endpoint);

    sys::error_code ec;
    Signal<void()> cancel_signal;

    call_lampshade_uncancellable<void>(
        _ex,
        yield[ec],
        &_pending_operations,
        cancel_signal,

        go_lampshade_dialer_init,
        _dialer_id,
        (char*) endpoint_string.c_str(),
        (void*) public_key_der.data(),
        public_key_der.size()
    );

    if (ec) {
        or_throw(yield, ec);
    }
}

GenericStream Dialer::dial(asio::yield_context yield, Signal<void()>& cancel)
{
    uint64_t connection_id = go_lampshade_connection_allocate();

    sys::error_code ec;
    call_lampshade_cancellable<void>(
        _ex,
        yield[ec],
        &_pending_operations,
        cancel,

        go_lampshade_dialer_dial,
        _dialer_id,
        connection_id
    );

    if (ec) {
        go_lampshade_connection_free(connection_id);
        return or_throw(yield, ec, GenericStream());
    }

    return GenericStream(LampshadeStream(_ex, connection_id));
}



Listener::Listener(const asio::executor& ex):
    _ex(ex),
    _listening(false)
{
    _listener_id = go_lampshade_listener_allocate();
}

Listener::~Listener()
{
    while (!_pending_operations.empty()) {
        auto& e = _pending_operations.front();
        e.cancel();
        /*
         * The handle will unlink itself in cancel(),
         * so there is no need to pop_front().
         */
    }

    if (_listening) {
        close();
    }
    go_lampshade_listener_free(_listener_id);
}

void Listener::listen(asio::ip::tcp::endpoint endpoint, std::string private_key_der, asio::yield_context yield)
{
    std::string endpoint_string;
    if (!endpoint.address().is_unspecified()) {
        endpoint_string += util::str(endpoint.address());
    }
    endpoint_string += ":";
    if (endpoint.port() != 0) {
        endpoint_string += std::to_string(endpoint.port());
    }

    sys::error_code ec;
    Signal<void()> cancel_signal;

    call_lampshade_uncancellable<void>(
        _ex,
        yield[ec],
        &_pending_operations,
        cancel_signal,

        go_lampshade_listener_create,
        _listener_id,
        (char*) endpoint_string.c_str(),
        (void*) private_key_der.data(),
        private_key_der.size()
    );

    if (ec) {
        or_throw(yield, ec);
    }

    _listening = true;
}

GenericStream Listener::accept(asio::yield_context yield)
{
    uint64_t connection_id = go_lampshade_connection_allocate();

    sys::error_code ec;
    Signal<void()> cancel_signal;

    call_lampshade_uncancellable<void>(
        _ex,
        yield[ec],
        &_pending_operations,
        cancel_signal,

        go_lampshade_listener_accept,
        _listener_id,
        connection_id
    );

    if (ec) {
        go_lampshade_connection_free(connection_id);
        return or_throw(yield, ec, GenericStream());
    }

    return GenericStream(LampshadeStream(_ex, connection_id));
}

void Listener::close()
{
    go_lampshade_listener_close(_listener_id, (void*) empty_lampshade_callback_void, nullptr);
    _listening = false;
}



struct Keypair {
    sys::error_code ec;
    std::string private_key;
    std::string public_key;
};

void generate_key_pair_callback(
    void* arg,
    int error,
    void* private_key,
    size_t private_key_length,
    void* public_key,
    size_t public_key_length
) {
    Keypair* buffer = reinterpret_cast<Keypair*>(arg);
    if (error) {
        LOG_ERROR("Lampshade: ", error);
        buffer->ec = boost::system::errc::make_error_code(boost::system::errc::no_message);
    } else {
        buffer->ec = sys::error_code();
        buffer->private_key = std::string((char *)private_key, private_key_length);
        buffer->public_key = std::string((char *)public_key, public_key_length);
    }
}

sys::error_code generate_key_pair(int bits, std::string& private_key, std::string& public_key)
{
    Keypair keypair;

    go_lampshade_generate_key(bits, (void *) generate_key_pair_callback, &keypair);

    if (!keypair.ec) {
        private_key = keypair.private_key;
        public_key = keypair.public_key;
    }
    return keypair.ec;
}

} // lampshade namespace
} // ouinet namespace
