#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast.hpp>
#include "namespaces.h"

namespace ipfs_cache { class Client; }

namespace ouinet {

class GenericConnection;

class ClientFrontEnd {
public:
    void serve( GenericConnection&
              , const http::request<http::string_body>&
              , std::shared_ptr<ipfs_cache::Client>&
              , boost::asio::yield_context);

    bool is_injector_proxying_enabled() const
    {
        return _injector_proxying_enabled;
    }

    bool is_ipfs_cache_enabled() const
    {
        return _ipfs_cache_enabled;
    }

private:
    bool _auto_refresh_enabled = true;
    bool _injector_proxying_enabled = true;
    bool _ipfs_cache_enabled = true;
};

} // ouinet namespace
