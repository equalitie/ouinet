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

std::expected<std::optional<http_response::Part>, sys::error_code>
Session::async_read_part(Async yield)
{
    auto destroyed = _destroyed.connect([&] { yield.cancel(); });

    if (!_reader) {
        return std::unexpected(asio::error::not_connected);
    }

    if (!_head_was_read) {
        _head_was_read = true;
        return _head;
    }

    auto part = _reader->async_read_part(yield);

    if (part && *part && _metrics) {
        if (auto size = payload_size(**part)) {
            _metrics->increment_transfer_size(size);
        }
    }

    if (!part || _reader->is_done()) {
        finish_metering(_metrics, part ? sys::error_code() : part.error());
    }

    if (!part) {
        return std::unexpected(part.error());
    }

    return *part;
}

Session::~Session() = default;

} // namespace ouinet
