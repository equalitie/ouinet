#pragma once

#include <boost/intrusive/list.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/optional.hpp>
#include <vector>
#include "../util/unique_function.h"
#include "../util/signal.h"
#include "../namespaces.h"

namespace ouinet { namespace stream {

template<class SourceStream>
class Fork {
    using IntrusiveHook = boost::intrusive::list_member_hook
        <boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

public:
    class Tine {
        friend class Fork;
        template<class T> friend class ouinet::stream::Fork;

        public:
        Tine(Fork&);
        Tine(const Tine&) = delete;
        Tine(Tine&&);
        Tine& operator=(Tine&&);

        template< class MutableBufferSequence
                , class Token>
        auto async_read_some(const MutableBufferSequence&, Token&&);


        template< class ConstBufferSequence
                , class Token>
        auto async_write_some(const ConstBufferSequence&, Token&&);

        void close();

        asio::io_service& get_io_service() {
            assert(fork);
            return fork->get_io_service();
        }

        asio::io_context::executor_type get_executor() {
            assert(fork);
            return fork->get_executor();
        }

        private:
        void flush_rx_handler(sys::error_code, size_t);

        private:
        IntrusiveHook hook;
        Fork* fork;
        util::unique_function<void(sys::error_code, size_t)> rx_handler;
        asio::const_buffer unread_data_buffer;
        std::vector<asio::mutable_buffer> rx_buffers;
        bool debug = false;
    };

    Fork(SourceStream, size_t buffer_size = 65536);
    Fork(const Fork&) = delete;
    Fork(Fork&&);
    Fork& operator=(Fork&&);

    ~Fork();

    void close();

    asio::io_service& get_io_service();
    asio::io_context::executor_type get_executor();

private:
    using Tines = boost::intrusive::list
        < Tine
        , boost::intrusive::member_hook<Tine, IntrusiveHook, &Tine::hook>
        , boost::intrusive::constant_time_size<false>
        >;

    struct ForkState : public std::enable_shared_from_this<ForkState> {
        using Work = asio::executor_work_guard<asio::io_context::executor_type>;

        SourceStream source;
        std::vector<uint8_t> rx_buffer;
        bool is_reading = false;

        bool read_again = false;

        size_t total_unread_data_size = 0;

        Cancel cancel;
        Tines tines;

        bool debug = false;

        ForkState(SourceStream s, size_t buffer_size)
            : source(std::move(s))
            , rx_buffer(buffer_size)
        {}

        ~ForkState() {
            if (debug) {
                std::cerr << this << " ForkState::~ForkState\n";
            }
        }

        void start_reading();

        void on_read(sys::error_code, size_t);

        boost::asio::io_context::executor_type get_executor() { return source.get_executor(); }

        void flush_rx_handlers(sys::error_code, size_t);

        asio::io_service& get_io_service() { return source.get_io_service(); }

        void on_tine_closed(size_t);
    };

private:
    std::shared_ptr<ForkState> _state;
    bool _debug = false;
};

template<class SourceStream>
inline
Fork<SourceStream>::Fork(SourceStream source, size_t buffer_size)
    : _state(std::make_shared<ForkState>(std::move(source), buffer_size))
{}

template<class SourceStream>
inline
Fork<SourceStream>::Fork(Fork&& other)
    : _state(std::move(other._state))
{
    if (!_state) return;

    for (auto& tine : _state->tines) {
        tine.fork = this;
    }
}

template<class SourceStream>
inline
Fork<SourceStream>& Fork<SourceStream>::operator=(Fork&& other)
{
    _state = std::move(other._state);

    if (!_state) return *this;

    for (auto& tine : _state->tines) {
        tine.fork = this;
    }

    return *this;
}

template<class SourceStream>
inline
void Fork<SourceStream>::ForkState::start_reading()
{
    if (debug) {
        std::cerr << this
                  << " ForkState::start_reading is_reading:" << is_reading
                  << " total_unread_data_size:" << total_unread_data_size << "\n";
    }

    assert(!read_again);

    if (!is_reading && total_unread_data_size == 0) {
        is_reading = true;

        source.async_read_some(asio::buffer(rx_buffer),
                [this, self = this->shared_from_this()]
                (const sys::error_code& ec, size_t size) mutable
                {
                    on_read(ec, size);
                    is_reading = false;
                    if (read_again) {
                        read_again = false;
                        get_executor().on_work_finished();

                        if (!cancel) start_reading();
                    }
                });
    }
    else {
        read_again = true;
        get_executor().on_work_started();
    }
}

template<class SourceStream>
inline
Fork<SourceStream>::Tine::Tine(Fork& fork)
    : fork(&fork)
{
    if (debug) {
        std::cerr << this << " Tine::Tine(Fork* " << this->fork << ")\n";
    }

    this->fork->_state->tines.push_back(*this);
}

template<class SourceStream>
inline
Fork<SourceStream>::Tine::Tine(Tine&& other)
    : fork(other.fork)
    , rx_handler(std::move(other.rx_handler))
    , unread_data_buffer(other.unread_data_buffer)
    , rx_buffers(std::move(other.rx_buffers))
    , debug(other.debug)
{
    hook.swap_nodes(other.hook);
    other.fork = nullptr;
    other.unread_data_buffer = asio::const_buffer();
}

template<class SourceStream>
inline
typename Fork<SourceStream>::Tine&
Fork<SourceStream>::Tine::operator=(Tine&& other)
{
    hook.swap_nodes(other.hook);

    fork(other.fork);
    other.fork = nullptr;

    rx_handler = std::move(other.rx_handler);

    unread_data_buffer = other.unread_data_buffer;
    other.unread_data_buffer = asio::const_buffer();

    rx_buffers = std::move(other.rx_buffers);

    debug = other.debug;

    return *this;
}

template<class SourceStream>
inline
void Fork<SourceStream>::Tine::flush_rx_handler(sys::error_code ec, size_t size)
{
    if (!rx_handler) return;

    assert(fork);

    fork->get_io_service().post(
        [ p = std::make_shared<decltype(rx_handler)>(std::move(rx_handler))
        , ec
        , size]
        () mutable {
            (*p)(ec, size);
        });
}

template<class SourceStream>
inline
void Fork<SourceStream>::Tine::close()
{
    if (!fork) return;

    if (debug) {
        std::cerr << this << " Tine::close()\n";
    }

    flush_rx_handler(asio::error::operation_aborted, 0);

    if (hook.is_linked()) hook.unlink();

    auto s = fork->_state;
    fork = nullptr;
    s->on_tine_closed(unread_data_buffer.size());
}

template<class SourceStream>
inline
void Fork<SourceStream>::ForkState::on_tine_closed(size_t s)
{
    if (debug) {
        std::cerr << this << " ForkState::on_tine_closed s:" << s
                  << " total_unread_data_size:" << total_unread_data_size
                  << " is_reading:" << is_reading
                  << " read_again:" << read_again
                  << "\n";
    }

    assert(s <= total_unread_data_size);

    total_unread_data_size -= s;
    if (total_unread_data_size != 0) return;
    if (is_reading) return;

    if (read_again) {
        read_again = false;
        get_executor().on_work_finished();
        if (!cancel) start_reading();
    }
}

template< class SourceStream>
template< class MutableBufferSequence
        , class Token>
inline
auto Fork<SourceStream>::Tine::async_read_some( const MutableBufferSequence& bs
                                              , Token&& token)
{
    if (debug) {
        std::cerr << this << " Tine::async_read_some\n";
    }

    assert(fork);
    assert(fork->_state);

    rx_buffers.clear();

    std::copy( asio::buffer_sequence_begin(bs)
             , asio::buffer_sequence_end(bs)
             , std::back_inserter(rx_buffers));

    asio::async_completion<Token, void(sys::error_code, size_t)> init(token);

    assert(!rx_handler);

    auto fork_state = fork->_state;

    if (unread_data_buffer.size()) {
        size_t size = asio::buffer_copy(rx_buffers, unread_data_buffer);
        unread_data_buffer += size;
        fork_state->total_unread_data_size -= size;

        fork->get_io_service().post(
                [ fork_state
                , size
                , h = std::move(init.completion_handler)
                ] () mutable {
                    if (fork_state->cancel)
                        h(asio::error::operation_aborted, 0);

                    h(sys::error_code(), size);

                    if (fork_state->cancel) return;

                    if (!fork_state->is_reading
                            && fork_state->read_again
                            && fork_state->total_unread_data_size == 0)
                    {
                        fork_state->read_again = false;
                        fork_state->get_executor().on_work_finished();

                        fork_state->start_reading();
                    }
                });
    }
    else {
        rx_handler = std::move(init.completion_handler);
        if (!fork_state->is_reading && !fork_state->read_again) {
            fork_state->start_reading();
        } else if (!fork_state->read_again) {
            fork_state->read_again = true;
            fork_state->get_executor().on_work_started();
        }
    }

    return init.result.get();
}

template< class SourceStream>
inline
void Fork<SourceStream>::ForkState::on_read(sys::error_code ec, size_t size)
{
    if (debug) {
        std::cerr << this << " ForkState::on_read " << total_unread_data_size
                  << " tines.size:" << tines.size() << "\n";
    }

    auto buf = asio::const_buffer(rx_buffer.data(), size);

    assert(!total_unread_data_size);

    typename Fork::Tines::iterator j;

    for (auto i = tines.begin(); i != tines.end(); i = j) {
        j = std::next(i);
        if (i->rx_handler) {
            auto h = std::move(i->rx_handler);
            size_t s = asio::buffer_copy(i->rx_buffers, buf);
            total_unread_data_size += size - s;
            i->unread_data_buffer = buf + s;
            h(ec, s);
        } else {
            total_unread_data_size += size;
            i->unread_data_buffer = buf;
        }
    }
}

template< class SourceStream>
template< class ConstBufferSequence
        , class Token>
inline
auto Fork<SourceStream>::Tine::async_write_some( const ConstBufferSequence&
                                               , Token&& token)
{
    assert(0 && "TODO");
}

template< class SourceStream>
inline
void Fork<SourceStream>::ForkState::flush_rx_handlers(sys::error_code, size_t)
{
    for (auto& tine : tines) {
        tine.flush_rx_handler(asio::error::operation_aborted, 0);
    }
}

template<class SourceStream>
inline
void Fork<SourceStream>::close()
{
    if (!_state) return;
    _state->cancel();
    _state->flush_rx_handlers(asio::error::operation_aborted, 0);
    _state = nullptr;
}

template<class SourceStream>
inline
Fork<SourceStream>::~Fork()
{
    if (_debug) {
        std::cerr << this << " ~Fork state:" << _state << "\n";
    }

    close();
}

template<class SourceStream>
inline
asio::io_service& Fork<SourceStream>::get_io_service()
{
    assert(_state);
    return _state->get_io_service();
}

template<class SourceStream>
inline
asio::io_context::executor_type Fork<SourceStream>::get_executor()
{
    assert(_state);
    return _state->get_executor();
}

}} // namespaces
