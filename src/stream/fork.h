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

    class ForkState;

public:
    class Tine {
        friend class Fork;
        template<class T> friend class ouinet::stream::Fork;

        public:
        Tine() = default;
        Tine(Fork&);
        Tine(const Tine&);
        Tine& operator=(const Tine&);
        Tine(Tine&&);
        Tine& operator=(Tine&&);

        template< class MutableBufferSequence
                , class Token>
        auto async_read_some(const MutableBufferSequence&, Token&&);


        template< class ConstBufferSequence
                , class Token>
        auto async_write_some(const ConstBufferSequence&, Token&&);

        void close();

        bool is_open() const;

        asio::io_service& get_io_service();

        asio::io_context::executor_type get_executor();

        private:
        void flush_rx_handler(sys::error_code, size_t);

        private:
        // ForkState knows about all the Tines through this hook
        IntrusiveHook hook;
        // ForkState runs the receive loop
        std::shared_ptr<ForkState> fork_state;
        // User provided rx_handler (or completion token)
        util::unique_function<void(sys::error_code, size_t)> rx_handler;
        // Points to some part of fork_state->rx_buffer that the user of this
        // Tine has not yet been notified about through the rx_handler and
        // rx_buffers.
        asio::const_buffer unread_data_buffer;
        // User provided buffers
        std::vector<asio::mutable_buffer> rx_buffers;
        // Whether to print debug output
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
        // Must be always equal to sum of each tine->unread_data_buffer.size()
        // We can only start new async_read operation when this is zero. I.e.
        // when all tines have read everything from rx_buffer that was received
        // during the previous completion of async_read call.
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
}

template<class SourceStream>
inline
Fork<SourceStream>& Fork<SourceStream>::operator=(Fork&& other)
{
    _state = std::move(other._state);
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
    : fork_state(fork._state)
{
    if (debug) {
        std::cerr << this << " Tine::Tine(ForkState* " << this->fork_state << ")\n";
    }

    fork_state->tines.push_back(*this);
}

template<class SourceStream>
inline
Fork<SourceStream>::Tine::Tine(const Tine& other)
    : fork_state(other.fork_state)
    , unread_data_buffer(other.unread_data_buffer)
{
    if (debug) {
        std::cerr << this << " Tine::Tine(Tine& " << &other << ")\n";
    }

    fork_state->tines.push_back(*this);
    fork_state->total_unread_data_size += unread_data_buffer.size();
}

template<class SourceStream>
inline
Fork<SourceStream>::Tine::Tine(Tine&& other)
    : fork_state(move(other.fork_state))
    , rx_handler(std::move(other.rx_handler))
    , unread_data_buffer(other.unread_data_buffer)
    , rx_buffers(std::move(other.rx_buffers))
    , debug(other.debug)
{
    hook.swap_nodes(other.hook);
    other.unread_data_buffer = asio::const_buffer();
}

template<class SourceStream>
inline
typename Fork<SourceStream>::Tine&
Fork<SourceStream>::Tine::operator=(Tine&& other)
{
    hook.swap_nodes(other.hook);

    fork_state = std::move(other.fork_state);
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

    assert(fork_state);

    fork_state->get_io_service().post(
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
    if (!fork_state) return;

    if (debug) {
        std::cerr << this << " Tine::close()\n";
    }

    flush_rx_handler(asio::error::operation_aborted, 0);

    if (hook.is_linked()) hook.unlink();

    auto s = std::move(fork_state);
    s->on_tine_closed(unread_data_buffer.size());
}

template<class SourceStream>
inline
bool Fork<SourceStream>::Tine::is_open() const
{
    if (!fork_state) return false;
    return fork_state->source.is_open();
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

    assert(fork_state);

    rx_buffers.clear();

    std::copy( asio::buffer_sequence_begin(bs)
             , asio::buffer_sequence_end(bs)
             , std::back_inserter(rx_buffers));

    asio::async_completion<Token, void(sys::error_code, size_t)> init(token);

    assert(!rx_handler);

    auto fs = fork_state;

    if (unread_data_buffer.size()) {
        size_t size = asio::buffer_copy(rx_buffers, unread_data_buffer);
        unread_data_buffer += size;
        fs->total_unread_data_size -= size;

        fs->get_io_service().post(
                [ fs
                , size
                , h = std::move(init.completion_handler)
                ] () mutable {
                    if (fs->cancel)
                        h(asio::error::operation_aborted, 0);

                    h(sys::error_code(), size);

                    if (fs->cancel) return;

                    if (!fs->is_reading
                            && fs->read_again
                            && fs->total_unread_data_size == 0)
                    {
                        fs->read_again = false;
                        fs->get_executor().on_work_finished();

                        fs->start_reading();
                    }
                });
    }
    else {
        rx_handler = std::move(init.completion_handler);
        if (!fs->is_reading && !fs->read_again) {
            fs->start_reading();
        } else if (!fs->read_again) {
            fs->read_again = true;
            fs->get_executor().on_work_started();
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
            // XXX: Shouldn't we post this to the io_service?
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

template<class SourceStream>
inline
asio::io_service& Fork<SourceStream>::Tine::get_io_service() {
    assert(fork_state);
    return fork_state->get_io_service();
}

template<class SourceStream>
inline
asio::io_context::executor_type Fork<SourceStream>::Tine::get_executor() {
    assert(fork_state);
    return fork_state->get_executor();
}


}} // namespaces
