#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/http/string_body.hpp>
#include "namespaces.h"
#include "cache/resource_id.h"


namespace ouinet {

//--------------------------------------------------------------------

class CachePeerRetrieveRequest {
public:
    CachePeerRetrieveRequest(const CachePeerRetrieveRequest&) = default;
    CachePeerRetrieveRequest(CachePeerRetrieveRequest&&) = default;

    http::verb method() const {
        return _method;
    }

    const cache::ResourceId& resource_id() const {
        return _resource_id;
    }

    const std::string& dht_group() const {
        return _dht_group;
    }

private:
    friend class CacheRetrieveRequest;

    CachePeerRetrieveRequest(http::verb method, cache::ResourceId resource_id, std::string dht_group) :
        _method(method),
        _resource_id(std::move(resource_id)),
        _dht_group(std::move(dht_group))
    {}

    http::verb _method;
    cache::ResourceId _resource_id;
    std::string _dht_group;
};

//--------------------------------------------------------------------

class CacheOuisyncRetrieveRequest {
public:
    CacheOuisyncRetrieveRequest(const CacheOuisyncRetrieveRequest&) = default;
    CacheOuisyncRetrieveRequest(CacheOuisyncRetrieveRequest&&) = default;

    http::verb method() const {
        return _method;
    }

    const cache::ResourceId& resource_id() const {
        return _resource_id;
    }

    const std::string& target() const {
        return _target;
    }

    const std::string& dht_group() const {
        return _dht_group;
    }

private:
    friend class CacheRetrieveRequest;

    CacheOuisyncRetrieveRequest(http::verb method, cache::ResourceId resource_id, std::string target, std::string dht_group) :
        _method(method),
        _resource_id(std::move(resource_id)),
        _target(std::move(target)),
        _dht_group(std::move(dht_group))
    {}

    http::verb _method;
    cache::ResourceId _resource_id;
    std::string _target;
    std::string _dht_group;
};

//--------------------------------------------------------------------

class CacheRetrieveRequest {
public:
    CacheRetrieveRequest(CacheRetrieveRequest const&) = default;
    CacheRetrieveRequest(CacheRetrieveRequest &&) = default;

    cache::ResourceId const& resource_id() const {
        return _resource_id;
    }

    CachePeerRetrieveRequest to_peer_request() const {
        return CachePeerRetrieveRequest(_method, _resource_id, _dht_group);
    }

    CacheOuisyncRetrieveRequest to_ouisync_request() const {
        return CacheOuisyncRetrieveRequest(_method, _resource_id, _target, _dht_group);
    }

private:
    friend class CacheRequest;

    CacheRetrieveRequest(http::verb method, cache::ResourceId resource_id, std::string dht_group, std::string target):
        _method(method),
        _resource_id(std::move(resource_id)),
        _dht_group(std::move(dht_group)),
        _target(std::move(target))
    {}

private:
    http::verb _method;
    cache::ResourceId _resource_id;
    std::string _dht_group;
    std::string _target;
};

//--------------------------------------------------------------------

class CacheInjectRequest {
public:
    CacheInjectRequest(const CacheInjectRequest&) = default;
    CacheInjectRequest(CacheInjectRequest&&) = default;

    http::verb method() const {
        return _header.method();
    }

    const cache::ResourceId& resource_id() const {
        return _resource_id;
    }

    const std::string& dht_group() const {
        return _dht_group;
    }

    void authorize(std::string_view credentials);
    void set_druid(std::string_view druid);

    template<class WriteStream>
    void async_write(WriteStream& con, asio::yield_context yield) {
        http::request<http::empty_body> msg(_header);
        msg.prepare_payload();
        http::async_write(con, msg, yield);
    }

    std::optional<const std::string_view> get_if_none_match_field() const {
        auto i = _header.find(http::field::if_none_match);
        if (i == _header.end()) return {};
        return i->value();
    }

    std::optional<const std::string_view> get_cache_control_field() const {
        auto i = _header.find(http::field::cache_control);
        if (i == _header.end()) return {};
        return i->value();
    }

private:
    friend class CacheRequest;

    CacheInjectRequest(http::request_header<> header, cache::ResourceId resource_id, std::string dht_group) :
        _header(std::move(header)),
        _resource_id(std::move(resource_id)),
        _dht_group(std::move(dht_group))
    {}

    http::request_header<> _header;
    cache::ResourceId _resource_id;
    std::string _dht_group;
};

//--------------------------------------------------------------------

class CacheRequest {
public:
    // TODO: This is only used in tests now, use it also when constructing the message.
    static const uint8_t HTTP_VERSION = 11;

    static boost::optional<CacheRequest> from(http::request_header<>, YieldContext);

    const http::request_header<>& header() const {
        return _header;
    }

    CacheInjectRequest to_inject_request() const;
    CacheRetrieveRequest to_retrieve_request() const;

    // https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/If-None-Match
    void set_if_none_match(std::string_view if_none_match);

    const std::string& dht_group() const { return _dht_group; }

    const cache::ResourceId& resource_id() const {
        return _resource_id;
    }

private:
    CacheRequest(http::request_header<> header, cache::ResourceId resource_id, std::string dht_group) :
        _header(std::move(header)),
        _resource_id(std::move(resource_id)),
        _dht_group(std::move(dht_group))
    {}

    http::request_header<> _header;
    cache::ResourceId _resource_id;
    std::string _dht_group;
};

//--------------------------------------------------------------------

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

    http::verb method() const {
        return _request.method();
    }

    void authorize(std::string_view credentials);
    void set_druid(std::string_view druid);

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

//--------------------------------------------------------------------

using PublicInjectorRequestAlternatives = std::variant<CacheInjectRequest, InsecureRequest>;

class PublicInjectorRequest : PublicInjectorRequestAlternatives {
private:
    using Base = PublicInjectorRequestAlternatives;

public:
    template<class Alternative>
    PublicInjectorRequest(Alternative&& alt) :
        Base(std::forward<Alternative>(alt))
    {}

    http::verb method() const;

    template<class WriteStream>
    void async_write(WriteStream& con, asio::yield_context yield) {
        std::visit(
            [&] (auto& alt) { alt.async_write(con, yield); },
            static_cast<Base&>(*this)
        );
    }

    void authorize(std::string_view credentials);
    void set_druid(std::string_view druid);
    bool is_inject_request() const;
};

} // namespace ouinet
