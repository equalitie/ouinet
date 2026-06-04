#pragma once

#include <boost/asio/ip/address.hpp>
#include "../../src/bittorrent/mainline_dht.h"

namespace ouinet::bittorrent {

// Spawn `count` DHT nodes listening on localhost and connected to each other. Wait for them to
// bootstrap and return them.
std::vector<std::unique_ptr<MainlineDht>> spawn_dht_nodes(size_t count, Async yield) {
    using boost::asio::ip::address_v4;

    auto exec = yield.get_executor();

    std::vector<asio_utp::udp_multiplexer> sockets;
    sockets.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        auto& socket = sockets.emplace_back(exec);

        sys::error_code ec;
        socket.bind(udp::endpoint(address_v4({127, 0, 0, 1}), 0), ec);
        BOOST_REQUIRE(!ec);
    }

    auto dns_resolver = std::make_shared<dns::Resolver>();

    std::vector<std::unique_ptr<MainlineDht>> dhts;
    dhts.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        std::set<bootstrap::Address> bootstrap_addrs;
        for (size_t j = 0; j < count; ++j) {
            if (i != j) {
                bootstrap_addrs.insert(sockets[j].local_endpoint());
            }
        }

        dhts.push_back(std::make_unique<MainlineDht>(
            exec,
            metrics::Client::noop().mainline_dht(),
            dns_resolver,
            (uint32_t) 0, // no mux_rx limit
            fs::path{}, // don't store contacts
            bootstrap::Config()
                .with_default(false)
                .with_extras(std::move(bootstrap_addrs)),
            util::LogPath(util::str("dht-node-", i))
        ));
    }

    for (size_t i = 0; i < count; ++i) {
        dhts[i]->set_peer_filter(PeerFilter::none);
        dhts[i]->add_endpoint(std::move(sockets[i]));
    }

    for (size_t i = 0; i < count; ++i) {
        dhts[i]->wait_all_ready(yield);
    }

    return dhts;
}

} // namespace ouinet::bittorrent
