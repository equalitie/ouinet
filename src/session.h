#pragma once

#include "generic_stream.h"
#include "response_reader.h"

//#include "util/part_io.h"
//#include <iostream>

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

    // Low-level session creation for partially read responses,
    // please consider using `create` below instead.
    Session(http_response::Head&& head, bool is_head_response, reader_uptr&& reader)
        : _head(std::move(head))
        , _reader(std::move(reader))
        , _is_head_response(is_head_response)
    {}

    // Construct the session and read response head
    static Session create(GenericStream, bool is_head_response, Cancel, asio::yield_context);

    template<class Reader>
    static Session create(std::unique_ptr<Reader>&&, bool is_head_response, Cancel, asio::yield_context);

          http_response::Head& response_header()       { return _head; }
    const http_response::Head& response_header() const { return _head; }

    boost::optional<http_response::Part>
    async_read_part(Cancel, asio::yield_context) override;

    template<class SinkStream>
    void flush_response(SinkStream&, Cancel&, asio::yield_context);
    template<class Handler>
    void flush_response(Cancel&, asio::yield_context, Handler&& h);

    bool is_done() const override {
        if (!_reader) return false;
        return _reader->is_done();
    }

    void close() override {
        if (!_reader) return;
        _reader->close();
        _reader = nullptr;
    }

    // The session object should not be used after calling this.
    reader_uptr release_reader() {
        if (!_reader) return nullptr;
        auto r = std::move(_reader);
        _reader = nullptr;
        return r;
    }

    bool keep_alive() const {
        return _head.keep_alive();
    }

    asio::executor get_executor() override {
        assert(_reader);
        return _reader->get_executor();
    }

    void debug() { _debug = true; }
    void debug_prefix(std::string s) { _debug_prefix = std::move(s); }

private:
    http_response::Head _head;
    reader_uptr _reader;
    bool _head_was_read = false;
    bool _is_head_response;
    bool _debug = false;
    std::string _debug_prefix;
};

inline
Session Session::create( GenericStream con
                       , bool is_head_response
                       , Cancel cancel
                       , asio::yield_context yield)
{
    assert(!cancel);

    reader_uptr reader = std::make_unique<http_response::Reader>(std::move(con));

    return Session::create(std::move(reader), is_head_response, cancel, yield);
}

template<class Reader>
inline
Session Session::create( std::unique_ptr<Reader>&& reader
                       , bool is_head_response
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
        // This is ok for the reader,
        // but it should be made explicit to code creating sessions.
        ec = http::error::end_of_stream;
    }

    if (ec) return or_throw<Session>(yield, ec);

    auto head = head_opt_part->as_head();

    if (!head) return or_throw<Session>(yield, http::error::unexpected_body);

    return Session{std::move(*head), is_head_response, std::move(reader)};
}

inline
boost::optional<http_response::Part>
Session::async_read_part(Cancel cancel, asio::yield_context yield)
{
    if (!_reader)
        return or_throw(yield, asio::error::not_connected, boost::none);

    if (!_head_was_read) {
        _head_was_read = true;
        return {{_head}};
    }
    return _reader->async_read_part(cancel, yield);
}

template<class Handler>
inline
void
Session::flush_response(Cancel& cancel,
                        asio::yield_context yield,
                        Handler&& h)
{
    if (!_reader)
        return or_throw(yield, asio::error::not_connected);

    assert(!_head_was_read);

    sys::error_code ec;

    _head_was_read = true;

    h(http_response::Part{_head}, cancel, yield[ec]);
    if (cancel) ec = asio::error::operation_aborted;
    return_or_throw_on_error(yield, cancel, ec);

    if (_is_head_response) return;

    while (true) {
        if (!_reader)
            return or_throw(yield, asio::error::not_connected);

        auto opt_part = _reader->async_read_part(cancel, yield[ec]);
        assert(ec != http::error::end_of_stream);
        return_or_throw_on_error(yield, cancel, ec);
        if (!opt_part) break;
        h(std::move(*opt_part), cancel, yield[ec]);
        if (cancel) ec = asio::error::operation_aborted;
        return_or_throw_on_error(yield, cancel, ec);
    }
}

template<class SinkStream>
inline
void
Session::flush_response(SinkStream& sink,
                        Cancel& cancel,
                        asio::yield_context yield)
{
    return flush_response(cancel, yield, [&sink] (auto&& part, auto& c, auto y) {
        part.async_write(sink, c, y);
    });
}

} // namespaces
