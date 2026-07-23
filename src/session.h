#pragma once

#include <expected>
#include "generic_stream.h"
#include "response_reader.h"
#include "util/watch_dog.h"
#include "util/async.h"
#include <boost/asio/spawn.hpp>
#include <cxx/metrics.h>
#include "api.h"

namespace ouinet {

enum PartModifier {
    DoNothing,
    // WebKit on iOS doesn't like chunk header extensions.
    RemoveChunkHeaderExtension,
};

class OUINET_COMMON_API Session : public http_response::AbstractReader {
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
    Session( http_response::Head&& head
           , std::optional<metrics::Request> metrics
           , bool is_head_response
           , reader_uptr&& reader)
        : _head(std::move(head))
        , _reader(std::move(reader))
        , _is_head_response(is_head_response)
        , _metrics(std::move(metrics))
    {}

    // Construct the session and read response head
    static std::expected<Session, sys::error_code>
    create(GenericStream, bool is_head_response, Async);

    static std::expected<Session, sys::error_code>
    create(GenericStream, bool is_head_response, std::optional<metrics::Request>, Async);

    template<class Reader>
    static std::expected<Session, sys::error_code>
    create(std::unique_ptr<Reader>&&, bool is_head_response, Async);

    template<class Reader>
    static std::expected<Session, sys::error_code>
    create(std::unique_ptr<Reader>&&, bool is_head_response, std::optional<metrics::Request>, Async);

    bool head_was_read() const { return _head_was_read; }

          http_response::Head& response_header()       { return _head; }
    const http_response::Head& response_header() const { return _head; }

    std::expected<std::optional<http_response::Part>, sys::error_code>
    async_read_part(Async) override;

    template<class SinkStream>
    std::expected<void, sys::error_code>
    flush_response(SinkStream&, Async, PartModifier part_modifier = PartModifier::DoNothing);

    template<class Handler>
    [[nodiscard]]
    std::expected<void, sys::error_code>
    flush_response(Async, Handler&& h);

    // The timeout will get reset with each successful send/recv operation,
    // so that the exchange does not get stuck for too long.
    template<class Handler, class TimeoutDuration>
    [[nodiscard]]
    std::expected<void, sys::error_code>
    flush_response(Async, Handler&& h, TimeoutDuration);


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

    AsioExecutor get_executor() override {
        assert(_reader);
        return _reader->get_executor();
    }

    ~Session();

private:
    static void finish_metering(std::optional<metrics::Request>& metrics, sys::error_code ec) {
        if (metrics) {
            metrics->finish(ec);
            metrics = {};
        }
    }

    static size_t payload_size(const http_response::Part& part) {
        if (auto body = part.as_body()) {
            return body->size();
        } else if (auto chunk_body = part.as_chunk_body()) {
            return chunk_body->size();
        }
        return 0;
    }

private:
    http_response::Head _head;
    reader_uptr _reader;
    bool _head_was_read = false;
    bool _is_head_response;
    std::optional<metrics::Request> _metrics;
    Cancel _destroyed;
};

//--------------------------------------------------------------------

template<class Reader>
inline
std::expected<Session, sys::error_code> Session::create(
    std::unique_ptr<Reader>&& reader,
    bool is_head_response,
    Async yield
)
{
    return Session::create( std::forward<std::unique_ptr<Reader>>(reader)
                          , is_head_response
                          , {}
                          , std::move(yield));
}

template<class Reader>
inline
std::expected<Session, sys::error_code> Session::create(
    std::unique_ptr<Reader>&& reader,
    bool is_head_response,
    std::optional<metrics::Request> metrics,
    Async yield
)
{
    auto head_opt_part = reader->async_read_part(yield);
    if (head_opt_part && !*head_opt_part) {
        // This is ok for the reader,
        // but it should be made explicit to code creating sessions.
        head_opt_part = std::unexpected(http::error::end_of_stream);
    }

    if (!head_opt_part) {
        finish_metering(metrics, head_opt_part.error());
        return std::unexpected(head_opt_part.error());
    }

    auto head = (**head_opt_part).as_head();

    if (!head) {
        auto ec = http::error::unexpected_body;
        finish_metering(metrics, ec);
        return std::unexpected(ec);
    }

    return Session(
        std::move(*head),
        std::move(metrics),
        is_head_response,
        std::move(reader)
    );
}

//--------------------------------------------------------------------

template<class Handler>
inline
std::expected<void, sys::error_code>
Session::flush_response(Async yield, Handler&& h)
{
    auto destroyed = _destroyed.connect([&yield] { yield.cancel(); });

    if (!_reader)
        return std::unexpected(asio::error::not_connected);

    assert(!_head_was_read);

    sys::error_code ec;

    _head_was_read = true;

    if (auto r = h(http_response::Part{_head}, yield); !r) {
        return std::unexpected(r.error());
    }

    if (_is_head_response) return {};

    while (true) {
        if (!_reader)
            return std::unexpected(asio::error::not_connected);

        auto opt_part_r = _reader->async_read_part(yield);

        if (!opt_part_r) {
            auto ec = opt_part_r.error();
            finish_metering(_metrics, ec);
            return std::unexpected(ec);
        }

        auto opt_part = std::move(*opt_part_r);

        if (!opt_part) {
            finish_metering(_metrics, ec);
            break;
        }

        if (_metrics) {
            if (auto size = payload_size(*opt_part)) {
                _metrics->increment_transfer_size(size);
            }
        }

        if (auto r = h(std::move(*opt_part), yield); !r) {
            return std::unexpected(r.error());
        }
    }

    return {};
}

template<class Handler, class TimeoutDuration>
inline
std::expected<void, sys::error_code>
Session::flush_response(Async yield, Handler&& h, TimeoutDuration timeout)
{
    Async timeout_yield = yield;

    try {
        auto op_wd = watch_dog( get_executor(), timeout
                              , [&timeout_yield] { timeout_yield.cancel(); });

        auto r = flush_response(timeout_yield, [&h, &op_wd, timeout] (auto&& part, auto y) -> std::expected<void, sys::error_code> {
            std::expected<void, sys::error_code> r = h(std::move(part), y);
            if (!r) return std::unexpected(r.error());
            op_wd.expires_after(timeout);  // the part was successfully forwarded
            return {};
        });

        if (!r) return std::unexpected(r.error());
    }
    catch (Async::Cancelled const&) {
        if (yield.is_cancelled()) throw;
        return std::unexpected(asio::error::timed_out);
    }

    return {};
}

template<class SinkStream>
inline
std::expected<void, sys::error_code>
Session::flush_response(SinkStream& sink, Async yield, PartModifier part_modifier)
{
    return flush_response(yield, [&sink, part_modifier] (auto&& part, auto y) {
        switch (part_modifier) {
            case PartModifier::DoNothing:
                return part.async_write(sink, y);
            case PartModifier::RemoveChunkHeaderExtension:
                if (auto chunk_hdr = part.as_chunk_hdr()) {
                    chunk_hdr->exts.clear();
                    return http_response::Part(std::move(*chunk_hdr)).async_write(sink, y);
                } else {
                    return part.async_write(sink, y);
                }
            default:
                assert(false && "unreachable");
                std::terminate();
        }
    });
}

} // namespace
