#pragma once

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/intrusive/list.hpp>

#include "../../generic_stream.h"
#include "../../namespaces.h"
#include "../../util/condition_variable.h"
#include "../../util/signal.h"

namespace ouinet {
namespace lampshade {

namespace detail {

struct Cancellable : public boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>
{
    virtual void cancel() = 0;
    virtual ~Cancellable() {}
};

} // detail namespace

class Dialer
{
    public:
    Dialer(asio::io_service& ios);
    ~Dialer();

    asio::io_service& get_io_service() { return _ios; }

    void init(asio::ip::tcp::endpoint endpoint, std::string public_key_der, asio::yield_context yield);
    GenericStream dial(asio::yield_context yield, Signal<void()>& cancel);

    protected:
    asio::io_service& _ios;
    boost::intrusive::list<detail::Cancellable, boost::intrusive::constant_time_size<false>> _pending_operations;
    uint64_t _dialer_id;
};

class Listener
{
    public:
    Listener(asio::io_service& ios);
    ~Listener();

    asio::io_service& get_io_service() { return _ios; }

    void listen(asio::ip::tcp::endpoint endpoint, std::string private_key_der, asio::yield_context yield);
    GenericStream accept(asio::yield_context yield);
    void close();

    protected:
    asio::io_service& _ios;
    boost::intrusive::list<detail::Cancellable, boost::intrusive::constant_time_size<false>> _pending_operations;
    uint64_t _listener_id;
    bool _listening;
};

sys::error_code generate_key_pair(int bits, std::string& private_key, std::string& public_key);

} // lampshade namespace
} // ouinet namespace
