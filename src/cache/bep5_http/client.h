#pragma once

#include "../abstract_cache.h"
#include "../../logger.h"
#include "../../util/crypto.h"
#include <boost/filesystem.hpp>

namespace ouinet {

namespace bittorrent {
    class MainlineDht;
}

class Session;

namespace cache {
namespace bep5_http {

class Client : public AbstractCache {
private:
    struct Impl;

public:
    static std::unique_ptr<Client>
    build( std::shared_ptr<bittorrent::MainlineDht>
         , util::Ed25519PublicKey cache_pk
         , fs::path cache_dir
         , asio::yield_context);

    Session load(const std::string& key, Cancel, Yield) override;

    void store( const std::string& key
              , Session&
              , Cancel
              , asio::yield_context) override;

    unsigned get_newest_proto_version() const override;

    ~Client();

    void        set_log_level(log_level_t);
    log_level_t get_log_level() const;

private:
    Client(std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> _impl;
};

}}} // namespaces
