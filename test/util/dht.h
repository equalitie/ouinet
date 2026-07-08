#pragma once

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <stdexcept>
#include "../../src/bittorrent/mainline_dht.h"
#include "../../src/bittorrent/mock_dht.h"
#include "../../src/util/str.h"
#include "asio_utp/udp_multiplexer.hpp"

namespace ouinet::bittorrent {

// Spawn nodes listening on the specified sockets and connect them to each other. Wait for them to
// bootstrap and return them.
std::vector<std::unique_ptr<MainlineDht>> spawn_dht_nodes(
    std::vector<asio_utp::udp_multiplexer> sockets,
    Async yield
) {
    auto dns_resolver = std::make_shared<dns::Resolver>();

    std::vector<std::unique_ptr<MainlineDht>> dhts;
    dhts.reserve(sockets.size());

    for (size_t i = 0; i < sockets.size(); ++i) {
        std::set<bootstrap::Address> bootstrap_addrs;
        for (size_t j = 0; j < sockets.size(); ++j) {
            if (i != j) {
                bootstrap_addrs.insert(sockets[j].local_endpoint());
            }
        }

        dhts.push_back(std::make_unique<MainlineDht>(
            yield.get_executor(),
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

    for (size_t i = 0; i < sockets.size(); ++i) {
        dhts[i]->set_peer_filter(PeerFilter::none);
        std::ignore = dhts[i]->add_endpoint(std::move(sockets[i]));
    }

    for (auto& dht : dhts) {
        dht->wait_all_ready(yield);
    }

    return dhts;
}

// Spawn `count` DHT nodes listening on localhost and connected to each other. Wait for them to
// bootstrap and return them.
std::vector<std::unique_ptr<MainlineDht>> spawn_dht_nodes(size_t count, Async yield) {
    using boost::asio::ip::address_v4;

    std::vector<asio_utp::udp_multiplexer> sockets;
    sockets.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        auto& socket = sockets.emplace_back(yield.get_executor());

        sys::error_code ec;
        socket.bind(udp::endpoint(address_v4({127, 0, 0, 1}), 0), ec);
        BOOST_REQUIRE(!ec);
    }

    return spawn_dht_nodes(std::move(sockets), yield);
}

// Which DHT implementation to use
enum class DhtImpl {
    // Read DHT running on the localhost
    real,
    // Mock DHT
    mock
};

inline std::ostream& operator << (std::ostream& os, DhtImpl impl) {
    switch (impl) {
        case DhtImpl::real: return os << "real";
        case DhtImpl::mock: return os << "mock";
        default: throw std::invalid_argument("invalid DHT impl");
    }
}

// Setup DHT for tests according to the DHT impl.
//
// For `DhtImpl::real` it returns:
//     - vector of `count` `MainlineDht` instances listening on the localhost and connected to each
//       other
//     - local endpoint of one of them
//     - nullptr
//
// For `DhtImpl::mock` it returns:
//     - empty vector
//     - dummy endpoint
//     - instance of `MockDht::Swarms`
//
std::tuple<
    std::vector<std::unique_ptr<MainlineDht>>,
    boost::asio::ip::udp::endpoint,
    std::shared_ptr<MockDht::Swarms>
>
setup_dht(DhtImpl impl, size_t count, Async yield) {
    switch (impl) {
    case DhtImpl::real: {
        auto nodes = spawn_dht_nodes(count, yield);
        auto endpoint = *nodes[0]->local_endpoints().begin();

        return std::make_tuple(std::move(nodes), endpoint, nullptr);
    }
    case DhtImpl::mock: {
        return std::make_tuple(
            std::vector<std::unique_ptr<MainlineDht>>(),
            boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0),
            std::make_shared<MockDht::Swarms>()
        );
    }
    default:
        throw std::invalid_argument("invalid DHT impl");
    }
}

// If `swarms` is `nullptr`, returns `nullptr`, otherwise returns a new `MockDht` instance.
std::shared_ptr<MockDht> mock_dht(
    std::string name,
    MockDht::Executor exec,
    std::shared_ptr<MockDht::Swarms> swarms
) {
    if (swarms) {
        return std::make_shared<MockDht>(std::move(name), std::move(exec), std::move(swarms));
    } else {
        return nullptr;
    }
}

// If `swarms` is `nullptr`, returns `nullopt`, otherwise returns a function that returns a new
// `MockDht` instance.
std::optional<std::function<std::shared_ptr<MockDht>()>>
mock_dht_builder(
    std::string name,
    MockDht::Executor exec,
    std::shared_ptr<MockDht::Swarms> swarms
) {
    if (swarms) {
        return [
            name = std::move(name),
            exec = std::move(exec),
            swarms = std::move(swarms)
        ] () {
            return std::make_shared<MockDht>(name, exec, swarms);
        };
    } else {
        return std::nullopt;
    }
}

} // namespace ouinet::bittorrent
