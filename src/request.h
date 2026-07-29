#pragma once

#include <boost/asio/spawn.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/http/string_body.hpp>
#include "namespaces.h"
#include "cache/resource_id.h"
#include "util/crypto_stream_key.h"
#include "util/async.h"
#include "cache_type.h"
#include "api.h"
#include <variant>

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

    const CryptoStreamKey& resource_key() const {
        return _resource_key;
    }

    const std::string& dht_group() const {
        return _dht_group;
    }

    InjectingCacheType cache_type() const {
        return _cache_type;
    }

private:
    friend class CacheRequest; // can construct

    CachePeerRetrieveRequest(http::verb method, InjectingCacheType cache_type, cache::ResourceId resource_id, CryptoStreamKey resource_key, std::string dht_group) :
        _method(method),
        _resource_id(std::move(resource_id)),
        _resource_key(std::move(resource_key)),
        _dht_group(std::move(dht_group)),
        _cache_type(cache_type)
    {}

    http::verb _method;
    cache::ResourceId _resource_id;
    CryptoStreamKey _resource_key;
    std::string _dht_group;
    InjectingCacheType _cache_type;
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

    const std::string& dht_group() const {
        return _dht_group;
    }

    friend std::ostream& operator<<(std::ostream&, CacheOuisyncRetrieveRequest const&);

private:
    friend class CacheRequest; // can construct

    CacheOuisyncRetrieveRequest(http::verb method, cache::ResourceId resource_id, std::string dht_group) :
        _method(method),
        _resource_id(std::move(resource_id)),
        _dht_group(std::move(dht_group))
    {}

    http::verb _method;
    cache::ResourceId _resource_id;
    std::string _dht_group;
};

//--------------------------------------------------------------------

class CacheRetrieveRequest {
public:
    using Alternatives = std::variant<
        CachePeerRetrieveRequest,
        CacheOuisyncRetrieveRequest
    >;

    CacheRetrieveRequest(CacheRetrieveRequest const&) = default;
    CacheRetrieveRequest(CacheRetrieveRequest &&) = default;

    CacheRetrieveRequest& operator=(CacheRetrieveRequest const&) = default;
    CacheRetrieveRequest& operator=(CacheRetrieveRequest &&) = default;

    template<class V>
    requires(
        !std::is_same_v<V, CacheRetrieveRequest> &&
        std::constructible_from<Alternatives, V>
    )
    CacheRetrieveRequest(V&& v) : _value(std::forward<V>(v)) {}

    template<class Visitor, class Self>
    decltype(auto) visit(this Self&& self, Visitor&& visitor) {
        return std::visit(std::forward<Visitor>(visitor), std::forward<Self>(self)._value);
    }

    const cache::ResourceId& resource_id() const {
        return visit([] (auto& r) -> cache::ResourceId const& { return r.resource_id(); });
    }

private:
    Alternatives _value;
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
    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write(WriteStream& con, Async yield) {
        http::request<http::empty_body> msg(_header);
        msg.prepare_payload();
        auto r = http::async_write(con, msg, yield);
        if (!r) return std::unexpected(r.error());
        return {};
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

    InjectingCacheType cache_type() const {
        return _cache_type;
    }

private:
    friend class CacheRequest;

    CacheInjectRequest(http::request_header<> header, InjectingCacheType cache_type, cache::ResourceId resource_id, std::string dht_group) :
        _header(std::move(header)),
        _resource_id(std::move(resource_id)),
        _dht_group(std::move(dht_group)),
        _cache_type(cache_type)
    {}

    http::request_header<> _header;
    cache::ResourceId _resource_id;
    std::string _dht_group;
    InjectingCacheType _cache_type;
};

//--------------------------------------------------------------------

class OUINET_CLIENT_API CacheRequest {
public:
    // TODO: This is only used in tests now, use it also when constructing the message.
    static const uint8_t HTTP_VERSION = 11;

    static std::optional<CacheRequest> from(CacheType, http::request_header<>);

    const http::request_header<>& header() const {
        return _header;
    }

    std::optional<CacheInjectRequest> to_inject_request() const;
    CacheRetrieveRequest to_retrieve_request() const;

    // https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/If-None-Match
    void set_if_none_match(std::string_view if_none_match);

    const std::string& dht_group() const { return _dht_group; }

    const cache::ResourceId& resource_id() const {
        return _resource_id;
    }

private:
    CacheRequest(http::request_header<> header, CacheType cache_type, cache::ResourceId resource_id, CryptoStreamKey const& resource_key, std::string dht_group) :
        _header(std::move(header)),
        _resource_id(std::move(resource_id)),
        _resource_key(resource_key),
        _dht_group(std::move(dht_group)),
        _cache_type(cache_type)
    {}

    http::request_header<> _header;
    cache::ResourceId _resource_id;
    CryptoStreamKey _resource_key;
    std::string _dht_group;
    CacheType _cache_type;
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
    static boost::optional<InsecureRequest> from(InjectingCacheType cache_type, http::request<http::string_body>);

    InsecureRequest(const InsecureRequest&) = default;
    InsecureRequest(InsecureRequest&&) = default;

    http::verb method() const {
        return _request.method();
    }

    void authorize(std::string_view credentials);
    void set_druid(std::string_view druid);

    template<class WriteStream>
    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write(WriteStream& con, Async yield) {
        _request.prepare_payload();
        auto r = http::async_write(con, _request, yield);
        if (!r) return std::unexpected(r.error());
        return {};
    }

    InjectingCacheType cache_type() const {
        return _cache_type;
    }

private:
    InsecureRequest(InjectingCacheType cache_type, http::request<http::string_body> request) :
        _cache_type(cache_type),
        _request(std::move(request))
    {}

    InjectingCacheType _cache_type;
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
    [[nodiscard]]
    std::expected<void, sys::error_code>
    async_write(WriteStream& con, Async yield) {
        return std::visit(
            [&] (auto& alt) { return alt.async_write(con, yield); },
            static_cast<Base&>(*this)
        );
    }

    InjectingCacheType cache_type() const {
        return std::visit([] (const auto& rq) { return rq.cache_type(); }, static_cast<const Base&>(*this));
    }

    void authorize(std::string_view credentials);
    void set_druid(std::string_view druid);
    bool is_inject_request() const;
};

} // namespace ouinet
