#include "session.h"

namespace ouinet {

Session Session::create( GenericStream con
                       , bool is_head_response
                       , Cancel cancel
                       , asio::yield_context yield)
{
    return Session::create(std::move(con), is_head_response, {}, std::move(cancel), yield);
}

Session Session::create( GenericStream con
                       , bool is_head_response
                       , std::optional<metrics::Request> metrics
                       , Cancel cancel
                       , asio::yield_context yield)
{
    assert(!cancel);

    reader_uptr reader = std::make_unique<http_response::Reader>(std::move(con));

    return Session::create(std::move(reader), is_head_response, std::move(metrics), cancel, yield);
}

boost::optional<http_response::Part>
Session::async_read_part(Cancel cancel, asio::yield_context yield)
{
    auto destroyed = _destroyed.connect([&cancel] { cancel(); });

    if (!_reader)
        return or_throw(yield, asio::error::not_connected, boost::none);

    if (!_head_was_read) {
        _head_was_read = true;
        return {{_head}};
    }

    sys::error_code ec;
    auto part = _reader->async_read_part(cancel, yield[ec]);

    if (!ec && part && _metrics) {
        if (auto size = payload_size(*part)) {
            _metrics->increment_transfer_size(size);
        }
    }

    if (ec || _reader->is_done()) {
        finish_metering(_metrics, ec);
    }

    if (ec) return or_throw(yield, ec, boost::none);

    return part;
}

Session::~Session() = default;

} // namespace ouinet
