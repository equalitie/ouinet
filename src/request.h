#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/http/string_body.hpp>
#include "namespaces.h"

namespace ouinet {

// Type safe way to ensure we're sending only stripped down requests to the
// injector and d-cache peers.
class CacheRequest {
public:
    static boost::optional<CacheRequest> from(http::request_header<>);

    CacheRequest(const CacheRequest&) = default;
    CacheRequest(CacheRequest&&) = default;

    const http::request_header<>& header() const {
        return _header;
    }

    void authorize(std::string_view credentials);
    void set_druid(std::string_view druid);
    void set_if_none_match(std::string_view if_none_match);
    bool can_inject() const { return true; }

    template<class WriteStream>
    void async_write(WriteStream& con, asio::yield_context yield) {
        http::request<http::empty_body> msg(_header);
        msg.prepare_payload();
        http::async_write(con, msg, yield);
    }

private:
    CacheRequest(http::request_header<> header) :
        _header(std::move(header)) {}

    http::request_header<> _header;
};

class InsecureRequest {
public:
    InsecureRequest(http::request<http::string_body>);

    InsecureRequest(const InsecureRequest&) = default;
    InsecureRequest(InsecureRequest&&) = default;

    const http::request_header<>& header() const {
        return _request;
    }

    void authorize(std::string_view credentials);
    void set_druid(std::string_view druid);
    bool can_inject() const { return false; }

    template<class WriteStream>
    void async_write(WriteStream& con, asio::yield_context yield) {
        _request.prepare_payload();
        http::async_write(con, _request, yield);
    }

private:
    http::request<http::string_body> _request;
};

using PublicInjectorRequestAlternatives = std::variant<CacheRequest, InsecureRequest>;

class PublicInjectorRequest : PublicInjectorRequestAlternatives {
private:
    using Base = PublicInjectorRequestAlternatives;

public:
    template<class Alternative>
    PublicInjectorRequest(Alternative&& alt) :
        Base(std::forward<Alternative>(alt))
    {}

    const http::request_header<>& header() const;

    template<class WriteStream>
    void async_write(WriteStream& con, asio::yield_context yield) {
        std::visit(
            [&] (auto& alt) { alt.async_write(con, yield); },
            static_cast<Base&>(*this)
        );
    }

    void authorize(std::string_view credentials);
    void set_druid(std::string_view druid);
    bool can_inject() const;
};

} // namespace ouinet
