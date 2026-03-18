#pragma once

#include <functional>
#include <set>
#include <bittorrent/mainline_dht.h>
#include "peer_lookup.h"

namespace std {
    template<> struct hash<ouinet::bittorrent::NodeID> {
        auto operator()(ouinet::bittorrent::NodeID const& a) const noexcept {
            return std::hash<std::string>{}(a.to_hex());
        }
    };
}

namespace ouinet::cache {

class DhtLookup : public PeerLookup<std::set<asio::ip::udp::endpoint>> {
    using udp = asio::ip::udp;

public:
    DhtLookup(DhtLookup&&) = delete;

    DhtLookup(std::weak_ptr<bittorrent::DhtBase> dht_w, std::string swarm_name)
        : PeerLookup(std::move(swarm_name), dht_w.lock()->get_executor())
        , _dht_w(dht_w)
    {
        _lookup_strategy_name = "DHT BEP5";
    }

    std::shared_ptr<bittorrent::DhtBase> get_dht_lock() {
        return _dht_w.lock();
    }

protected:
    Ret do_lookup(Cancel& c, asio::yield_context y) override {
        auto dht = _dht_w.lock();
        assert(dht);

        if (!dht)
            return or_throw(y, asio::error::operation_aborted, Ret{});

        sys::error_code ec;
        auto eps = dht->tracker_get_peers(infohash(), c, y[ec]);
        return or_throw(y, ec, std::move(eps));
    }

private:
    std::weak_ptr<bittorrent::DhtBase> _dht_w;
};

} // namespaces
