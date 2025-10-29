#pragma once

#include <boost/beast/http/message.hpp>
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

    template<class WriteStream>
    void async_write(WriteStream& con, asio::yield_context yield) const {
        http::request<http::empty_body> msg(_header, http::empty_body());
        msg.prepare_payload();
        http::async_write(con, msg, yield);
    }

private:
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

    template<class WriteStream>
    void async_write(WriteStream& con, asio::yield_context yield) const {
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

    template<class WriteStream>
    void async_write(WriteStream& con, asio::yield_context yield) const {
        std::visit(
            [&] (auto&& alt) { alt.async_write(con, yield); },
            static_cast<const Base&>(*this)
        );
    }

    void authorize(std::string_view credentials);

    void set_druid(std::string_view druid);
};

} // namespace ouinet
