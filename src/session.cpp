#include "session.h"

namespace ouinet {

std::expected<Session, sys::error_code>
Session::create(GenericStream con, bool is_head_response, Async yield) {
    return Session::create(std::move(con), is_head_response, {}, std::move(yield));
}

std::expected<Session, sys::error_code>
Session::create(
    GenericStream con,
    bool is_head_response,
    std::optional<metrics::Request> metrics,
    Async yield
) {
    return Session::create(
        std::make_unique<http_response::Reader>(std::move(con)),
        is_head_response,
        std::move(metrics),
        std::move(yield)
    );
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
