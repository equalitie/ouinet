#pragma once

#include "../abstract_cache.h"
#include <boost/filesystem.hpp>

namespace ouinet {
namespace cache {
namespace bep5_http {

class Client : public AbstractCache {
private:
    struct Impl;

public:
    static std::unique_ptr<Client>
    build(asio::io_service&, fs::path cache_dir, asio::yield_context);

    void load( const std::string& key
             , GenericStream& sink
             , Cancel
             , Yield) override;

    void store( const std::string& key
              , const http::response_header<>&
              , GenericStream& response_body
              , Cancel
              , asio::yield_context) override;

    ~Client();

private:
    Client(std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> _impl;
};

}}} // namespaces
