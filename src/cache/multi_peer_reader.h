#pragma once

#include <set>
#include <boost/asio/ip/udp.hpp>
#include "../response_reader.h"
#include "../namespaces.h"
#include "../bittorrent/dht.h"
#include "dht_lookup.h"
#include "../util/async_generator.h"
#include "../session.h"

namespace ouinet { namespace cache {

class MultiPeerReader : public http_response::AbstractReader {
private:
    class Peer;
    class Peers;

    struct Connection {
        asio::ip::udp::endpoint endpoint;
        Session session;
    };

    using ConnectionGenerator = util::AsyncGenerator<Connection>;

public:
    MultiPeerReader( asio::executor ex
                   , util::Ed25519PublicKey cache_pk
                   , std::set<asio::ip::udp::endpoint> local_peers
                   , std::string key
                   , std::shared_ptr<bittorrent::MainlineDht> dht
                   , std::string dht_group
                   , std::shared_ptr<DhtLookup> dht_lookup
                   , std::shared_ptr<unsigned> newest_proto_seen
                   , const std::string& dbg_tag);

    boost::optional<http_response::Part> async_read_part(Cancel, asio::yield_context) override;

    bool is_done() const override;
    bool is_open() const override;
    void close() override;

    ~MultiPeerReader();

private:
    static
    std::unique_ptr<ConnectionGenerator>
    make_connection_generator( asio::executor exec
                             , std::set<asio::ip::udp::endpoint> local_peers
                             , util::Ed25519PublicKey cache_pk
                             , const std::string& key
                             , const std::string& dht_group
                             , std::shared_ptr<bittorrent::MainlineDht> dht
                             , std::shared_ptr<DhtLookup> dht_lookup
                             , std::shared_ptr<unsigned> newest_proto_seen
                             , const std::string& dbg_tag);
private:
    Cancel _lifetime_cancel;

    bool _closed = false;
    std::unique_ptr<Peers> _peers;
    Peer* _chosen_peer = nullptr;
    std::string _dbg_tag;
};

}}
