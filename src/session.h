#pragma once

#include "generic_stream.h"
#include "response_reader.h"

namespace ouinet {

class Session : public http_response::AbstractReader {
public:
    using reader_uptr = std::unique_ptr<http_response::AbstractReader>;

public:
    Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Session(Session&&) = default;
    Session& operator=(Session&&) = default;

    // Construct the session and read response head
    static Session create(GenericStream, Cancel, asio::yield_context);

    template<class Reader>
    static Session create(std::unique_ptr<Reader>&&, Cancel, asio::yield_context);

          http_response::Head& response_header()       { return _head; }
    const http_response::Head& response_header() const { return _head; }

    boost::optional<http_response::Part>
    async_read_part(Cancel, asio::yield_context) override;

    template<class SinkStream>
    void flush_response(SinkStream&, Cancel&, asio::yield_context);
    template<class Handler>
    void flush_response(Cancel&, asio::yield_context, Handler&& h);

    bool is_open() const override {
        return _reader->is_open();
    }

    void close() override {
        if (!_reader) return;
        if (_reader->is_open()) _reader->close();
    }

    bool keep_alive() const {
        return _head.keep_alive();
    }

    bool is_done() const override {
        if (!_head_was_read) return false;
        if (!_reader) return true;
        return _reader->is_done();
    }

private:
    Session(http_response::Head&& head, reader_uptr&& reader)
        : _head(std::move(head))
        , _reader(std::move(reader))
    {}

private:
    http_response::Head _head;
    reader_uptr _reader;
    bool _head_was_read = false;
};

inline
Session Session::create(GenericStream con, Cancel cancel, asio::yield_context yield)
{
    assert(!cancel);

    reader_uptr reader = std::make_unique<http_response::Reader>(std::move(con));

    return Session::create(std::move(reader), cancel, yield);
}

template<class Reader>
inline
Session Session::create( std::unique_ptr<Reader>&& reader
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

inline
boost::optional<http_response::Part>
Session::async_read_part(Cancel cancel, asio::yield_context yield)
{
    if (!_head_was_read) {
        _head_was_read = true;
        return {{_head}};
    }
    return _reader->async_read_part(cancel, yield);
}

template<class SinkStream>
inline
void
Session::flush_response(SinkStream& sink,
                        Cancel& cancel,
                        asio::yield_context yield)
{
    assert(!_head_was_read);
    sys::error_code ec;

    _head_was_read = true;
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

template<class Handler>
inline
void
Session::flush_response(Cancel& cancel,
                        asio::yield_context yield,
                        Handler&& h)
{
    assert(!_head_was_read);

    sys::error_code ec;

    _head_was_read = true;
    h(http_response::Part{std::move(_head)}, cancel, yield[ec]);
    return_or_throw_on_error(yield, cancel, ec);

    while (true) {
        auto opt_part = _reader->async_read_part(cancel, yield[ec]);
        assert(ec != http::error::end_of_stream);
        return_or_throw_on_error(yield, cancel, ec);
        if (!opt_part) break;
        h(std::move(*opt_part), cancel, yield[ec]);
        return_or_throw_on_error(yield, cancel, ec);
    }
}

} // namespaces
