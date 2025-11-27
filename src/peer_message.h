#pragma once

#include <boost/beast/http/message.hpp>
#include "generic_stream.h"
#include "util/yield.h"
#include "cache/resource_id.h"
#include "http_util.h"

namespace ouinet {

// Peer wants to connect to the Injector using us as a Bridge
class PeerConnectRequest {};

// Peer wants something from our cache
class PeerCacheRequest {
public:
    http::verb method() const { return _method; }

    bool keep_alive() const {
        return _keep_alive;
    }

    const cache::ResourceId& resource_id() const {
        return _resource_id;
    }

    std::optional<util::HttpRequestByteRange> range() const {
        return _range;
    }

    friend std::ostream& operator<<(std::ostream& os, const PeerCacheRequest& r) {
        r.print(os);
        return os;
    }

private:
    friend class PeerRequest;

    PeerCacheRequest(
            http::verb method,
            bool keep_alive,
            cache::ResourceId(resource_id),
            std::optional<util::HttpRequestByteRange> range) : 
        _method(method),
        _keep_alive(keep_alive),
        _resource_id(std::move(resource_id)),
        _range(range)
    {}

    void print(std::ostream&) const;

private:
    http::verb _method;
    bool _keep_alive;
    cache::ResourceId _resource_id;
    std::optional<util::HttpRequestByteRange> _range;
};

using PeerRequestVariants = std::variant<std::monostate, PeerConnectRequest, PeerCacheRequest>;

class PeerRequest : public PeerRequestVariants {
public:
    using Base = PeerRequestVariants;

    PeerRequest() = default;

    template<class M>
    PeerRequest(M m): Base(std::move(m)) {}

    PeerRequest static async_read(GenericStream&, YieldContext yield);
};

enum PeerRequestError {
    success = 0,
    invalid_method,
    invalid_protocol_version,
    invalid_target,
    invalid_range,
};

sys::error_category const& peer_request_error_category();

inline
sys::error_code make_error_code(PeerRequestError e) {
    return sys::error_code(static_cast<int>(e), peer_request_error_category());
}

} // namespace ouinet

namespace boost::system {
    template<> struct is_error_code_enum<::ouinet::PeerRequestError>: std::true_type{};
} // namespace boost::system
