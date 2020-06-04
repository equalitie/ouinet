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
    class Peer;

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

    ~MultiPeerReader() {
        _lifetime_cancel();
    }

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
    asio::executor _exec;
    std::shared_ptr<bittorrent::MainlineDht> _dht;
    std::set<asio::ip::udp::endpoint> _local_peers;
    std::string _key;
    std::string _dht_group;
    std::shared_ptr<DhtLookup> _dht_lookup;
    std::shared_ptr<unsigned> _newest_proto_seen;
    Cancel _lifetime_cancel;

    bool _closed = false;
    std::unique_ptr<ConnectionGenerator> _connection_generator;
    boost::optional<Connection> _chosen_connection;
};

}}
