#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/http/string_body.hpp>
#include "namespaces.h"


namespace ouinet {

// Cache request sent either to the origin through injector or to peers.
//
// * Injector and peers can see the request in plain text
// * Non white listed headers are removed
// * Only GET requests are allowed
// * Request body is removed (if present)
// * GET arguments (`?...`) are removed from the request target
// * Requests containing the http_::request_private_hdr field are not allowed
// * Requests must contain the http_::request_group_hdr unless on Apple devices
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

    const std::string& dht_group() const {
        return _dht_group;
    }

    template<class WriteStream>
    void async_write(WriteStream& con, asio::yield_context yield) {
        http::request<http::empty_body> msg(_header);
        msg.prepare_payload();
        http::async_write(con, msg, yield);
    }

private:
    CacheRequest(http::request_header<> header, std::string dht_group) :
        _header(std::move(header)),
        _dht_group(std::move(dht_group))
    {}

    http::request_header<> _header;
    std::string _dht_group;
};

// Sent through the injector and to the origin when the original request from
// the user agent is not a secure HTTPS (i.e. http://...). In such case the
// injector can't create a secure connection to the origin.
//
// * The injector can see the request
// * All `X-Ouinet...` headers are removed from the request
class InsecureRequest {
public:
    static boost::optional<InsecureRequest> from(http::request<http::string_body>);

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
    InsecureRequest(http::request<http::string_body> request) :
        _request(std::move(request))
    {}

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
