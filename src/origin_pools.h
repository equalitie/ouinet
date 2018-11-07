#pragma once

#include "connection_pool.h"

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

    using Connection = ConnectionPool<>::Connection;

public:
    std::unique_ptr<Connection> get_connection(const RequestHdr& rq);

    void insert_connection(const RequestHdr& rq, std::unique_ptr<Connection>);

private:
    boost::optional<PoolId> make_pool_id(const RequestHdr& hdr);

private:
    std::map<PoolId, ConnectionPool<>> _pools;
};

inline
std::unique_ptr<OriginPools::Connection>
OriginPools::get_connection(const RequestHdr& rq)
{
    auto opt_pool_id = make_pool_id(rq);

    assert(opt_pool_id);

    if (!opt_pool_id) return nullptr;
    
    auto pool_i = _pools.find(*opt_pool_id);

    if (pool_i == _pools.end()) return nullptr;

    auto ret = pool_i->second.pop_front();

    if (pool_i->second.empty()) {
        _pools.erase(pool_i);
    }

    return ret;
}

inline
void
OriginPools::insert_connection( const RequestHdr& rq
                              , std::unique_ptr<Connection> con)
{
    if (!con) return;

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
