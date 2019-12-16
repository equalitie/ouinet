#pragma once

#include "generic_stream.h"
#include "response_reader.h"

namespace ouinet {

class Session {
public:
    Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Session(Session&&) = default;
    Session& operator=(Session&&) = default;

    Session(GenericStream con)
        : _reader(new http_response::Reader(std::move(con)))
    {}

    const http_response::Head* response_header() const {
        if (!_reader) return nullptr;
        if (!_head) return nullptr;
        return &*_head;
    }

    http_response::Head* response_header() {
        if (!_reader) return nullptr;
        if (!_head) return nullptr;
        return &*_head;
    }

    http_response::Head* read_response_header(Cancel&, asio::yield_context);

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
        assert(_head);
        if (!_head) return false;
        return _head->keep_alive();
    }

private:
    boost::optional<http_response::Head> _head;
    std::unique_ptr<http_response::Reader> _reader;
};

inline
http_response::Head*
Session::read_response_header(Cancel& cancel, asio::yield_context yield)
{
    using http_response::Head;

    assert(!cancel);

    if (!_reader) {
        return or_throw<Head*>(yield, asio::error::bad_descriptor);
    }

    if (_head) return &*_head;

    sys::error_code ec;

    auto part = _reader->async_read_part(cancel, yield[ec]);

    if (cancel) {
        assert(ec == asio::error::operation_aborted);
        ec = asio::error::operation_aborted;
    }

    if (!part) {
        assert(ec);
        ec = http::error::unexpected_body;
    }

    if (ec) return or_throw<Head*>(yield, ec, nullptr);

    auto head = part->as_head();

    if (!head) return or_throw<Head*>(yield, http::error::unexpected_body, nullptr);

    _head = std::move(*head);

    return &*_head;
}

template<class SinkStream>
inline
void
Session::flush_response(SinkStream& sink,
                        Cancel& cancel,
                        asio::yield_context yield)
{
    sys::error_code ec;

    if (_head) {
        http_response::Head* p = &*_head;
        p->async_write(sink, cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
    }

    while (true) {
        auto opt_part = _reader->async_read_part(cancel, yield[ec]);
        assert(ec != http::error::end_of_stream);
        return_or_throw_on_error(yield, cancel, ec);
        if (!opt_part) break;
        opt_part->async_write(sink, cancel, yield[ec]);
    }
}

} // namespaces
