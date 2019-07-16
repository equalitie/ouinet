#pragma once

#include "connection_pool.h"
#include <boost/optional.hpp>

namespace ouinet {

class OriginPools {
private:
    using RequestHdr = beast::http::header<true>; 

public:
    struct PoolId {
        bool is_ssl;
        std::string host;

        bool operator<(const PoolId& other) const {
            return tie(is_ssl, host) < tie(other.is_ssl, other.host);
        }
    };

    using Connection = ConnectionPool<bool>::Connection;

public:
    Connection wrap(const RequestHdr&, GenericStream);

    boost::optional<Connection> get_connection(const RequestHdr& rq);

    void insert_connection(const RequestHdr& rq, Connection);

private:
    boost::optional<PoolId> make_pool_id(const RequestHdr& hdr);

private:
    std::map<PoolId, ConnectionPool<bool>> _pools;
};

inline
boost::optional<OriginPools::Connection>
OriginPools::get_connection(const RequestHdr& rq)
{
    auto opt_pool_id = make_pool_id(rq);

    assert(opt_pool_id);

    if (!opt_pool_id) return boost::none;

    auto pool_i = _pools.find(*opt_pool_id);

    if (pool_i == _pools.end()) return boost::none;

    if (pool_i->second.empty()) {
        _pools.erase(pool_i);
        return boost::none;
    }

    auto ret = pool_i->second.pop_front();

    if (pool_i->second.empty()) {
        _pools.erase(pool_i);
    }

    return ret;
}

inline
OriginPools::Connection
OriginPools::wrap(const RequestHdr& rq, GenericStream connection)
{
    auto opt_pool_id = make_pool_id(rq);

    assert(opt_pool_id);
    if (!opt_pool_id) return Connection();

    return _pools[*opt_pool_id].wrap(std::move(connection));
}

inline
void
OriginPools::insert_connection(const RequestHdr& rq, Connection con)
{
    auto opt_pool_id = make_pool_id(rq);

    assert(opt_pool_id);

    if (!opt_pool_id) return;

    _pools[*opt_pool_id].push_back(std::move(con));
}

inline
boost::optional<OriginPools::PoolId>
OriginPools::make_pool_id(const RequestHdr& hdr)
{
    auto host = hdr[http::field::host];

    assert(!host.empty());

    if (host.empty()) return boost::none;

    bool is_ssl = hdr.target().starts_with("https:");

    // TODO: Can we avoid converting to string?
    return PoolId{is_ssl, host.to_string()};
}

} // namespace
