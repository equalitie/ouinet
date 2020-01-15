#pragma once

#include "generic_stream.h"
#include "response_reader.h"

namespace ouinet {

class Session {
public:
    using reader_uptr = std::unique_ptr<http_response::Reader>;

public:
    Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Session(Session&&) = default;
    Session& operator=(Session&&) = default;

    // Construct the session and read response head
    static Session create(GenericStream, Cancel, asio::yield_context);
    static Session create(reader_uptr&&, Cancel, asio::yield_context);

          http_response::Head& response_header()       { return _head; }
    const http_response::Head& response_header() const { return _head; }

    template<class SinkStream>
    void flush_response(SinkStream&, Cancel&, asio::yield_context);

    bool is_open() const {
        return _reader->is_open();
    }

    void close() {
        if (!_reader) return;
        if (_reader->is_open()) _reader->close();
    }

    bool keep_alive() const {
        return _head.keep_alive();
    }

private:
    Session(http_response::Head&& head, reader_uptr&& reader)
        : _head(std::move(head))
        , _reader(std::move(reader))
    {}

private:
    http_response::Head _head;
    reader_uptr _reader;
};

inline
Session Session::create(GenericStream con, Cancel cancel, asio::yield_context yield)
{
    assert(!cancel);

    auto reader = std::make_unique<http_response::Reader>(std::move(con));

    return Session::create(std::move(reader), cancel, yield);
}

inline
Session Session::create( reader_uptr&& reader
                       , Cancel cancel, asio::yield_context yield)
{
    assert(!cancel);

    sys::error_code ec;

    auto head_opt_part = reader->async_read_part(cancel, yield[ec]);

    if (cancel) {
        assert(ec == asio::error::operation_aborted);
        ec = asio::error::operation_aborted;
    }

    if (!ec && !head_opt_part) {
        assert(ec);
        ec = http::error::unexpected_body;
    }

    if (ec) return or_throw<Session>(yield, ec);

    auto head = head_opt_part->as_head();

    if (!head) return or_throw<Session>(yield, http::error::unexpected_body);

    return Session{std::move(*head), std::move(reader)};
}

template<class SinkStream>
inline
void
Session::flush_response(SinkStream& sink,
                        Cancel& cancel,
                        asio::yield_context yield)
{
    sys::error_code ec;

    _head.async_write(sink, cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec);

    while (true) {
        auto opt_part = _reader->async_read_part(cancel, yield[ec]);
        assert(ec != http::error::end_of_stream);
        return_or_throw_on_error(yield, cancel, ec);
        if (!opt_part) break;
        opt_part->async_write(sink, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
    }
}

} // namespaces
