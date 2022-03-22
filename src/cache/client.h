#pragma once

#include "../../response_reader.h"
#include "../../util/crypto.h"
#include "../../util/yield.h"

namespace ouinet {

namespace bittorrent {
    class MainlineDht;
}

class Session;

namespace cache {

class LocalClient;

class Client {
private:
    struct Impl;

public:
    static std::unique_ptr<Client>
    build( std::shared_ptr<bittorrent::MainlineDht> dht
         , util::Ed25519PublicKey cache_pk
         , std::shared_ptr<LocalClient> local_client
         , asio::yield_context yield);

    // This may add a response source header.
    Session load( const std::string& key
                , const std::string& dht_group
                , bool is_head_request
                , Cancel
                , Yield);

    void store( const std::string& key
              , const std::string& dht_group
              , http_response::AbstractReader&
              , Cancel
              , asio::yield_context);

    // Get the newest protocol version that has been seen in the network
    // (e.g. to warn about potential upgrades).
    unsigned get_newest_proto_version() const;

    ~Client();

private:
    Client(std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> _impl;
};

}} // namespaces
