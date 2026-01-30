#include <boost/asio/spawn.hpp>
#include <boost/asio/ip/multicast.hpp>
#include "local_peer_discovery.h"
#include <util/random.h>
#include <util/handler_tracker.h>
#include <parse/number.h>
#include <parse/endpoint.h>
#include <logger.h>
#include <async_sleep.h>

using namespace ouinet;
using namespace std;

using udp = asio::ip::udp;
using boost::string_view;
namespace ip = asio::ip;

// Arbitrarily chosen so as to not clash with any from:
// https://www.iana.org/assignments/multicast-addresses/multicast-addresses.xhtml
// TODO: IPv6
static const udp::endpoint multicast_ep(ip::make_address("237.176.57.49"), 37391);

static const string MSG_PREFIX = "OUINET-LPD-V0:";
static const string MSG_QUERY_CMD = "QUERY:";
static const string MSG_REPLY_CMD = "REPLY:";
static const string MSG_BYE_CMD   = "BYE:";

static bool consume(boost::string_view& sv, boost::string_view what) {
    if (!sv.starts_with(what)) {
        return false;
    }
    sv.remove_prefix(what.size());
    return true;
}

static
boost::optional<set<udp::endpoint>>
consume_endpoints(boost::string_view& sv, asio::ip::address from) {
    set<udp::endpoint> ret;
    while (!sv.empty()) {
        auto opt_ep = parse::endpoint<udp>(sv);
        if (!opt_ep) return boost::none;
        if (!consume(sv, ";")) return boost::none;
        if (opt_ep->address().is_unspecified()) {
            opt_ep->address(from);
        }
        ret.insert(*opt_ep);
    }
    return ret;
}

using PeerId = uint64_t;

namespace std {
    inline std::ostream& operator<<(std::ostream& os, std::set<udp::endpoint> const& map)
    {
        os << "{ ";
        for (auto comma{map.size()}; auto const& p : map)
            os << '\'' << p << '\'' << (--comma ? ", " : " ");
        return os << "}";
    }
}

struct LocalPeerDiscovery::Impl
    : public std::enable_shared_from_this<LocalPeerDiscovery::Impl>
{
    struct Peer {
        udp::endpoint discovery_ep;
        set<udp::endpoint> advertised_eps;
    };

    AsioExecutor _ex;
    udp::socket _socket;
    PeerId _id;
    set<udp::endpoint> _advertised_eps;
    map<PeerId, Peer> _peers;
    Cancel _cancel;

    Impl( const AsioExecutor& ex
        , uint64_t id
        , set<udp::endpoint> advertised_eps)
        : _ex(ex)
        , _socket(ex)
        , _id(id)
        , _advertised_eps(move(advertised_eps))
    {
    }

    void start()
    {
        sys::error_code ec;

        LOG_INFO("LocalPeerDiscovery: starting with advertised endpoints ", _advertised_eps);

        _socket.open(udp::v4());
        _socket.set_option(udp::socket::reuse_address(true));
        _socket.bind({asio::ip::address_v4::any(), multicast_ep.port()}, ec);

        if (!ec) _socket.set_option(ip::multicast::join_group(multicast_ep.address()), ec);

        if (ec) {
            LOG_ERROR("LocalPeerDiscovery: Failed to bind recv socket;"
                      " ec=", ec);
            return;
        } else {
            LOG_DEBUG("LocalPeerDiscovery: bound to ", multicast_ep);
        }

        start_listening_to_broadcast();
        broadcast_search_query();
    }

    void say_bye() {
        sys::error_code ec;
        _socket.send_to(asio::buffer(bye_message()), multicast_ep, 0, ec);
    }

    void broadcast_search_query() {
        auto self = shared_from_this();
        TRACK_SPAWN(_ex, ([this, self] (asio::yield_context yield) {
            sys::error_code ec;
            udp::endpoint ep = multicast_ep;
            _socket.async_send_to( asio::buffer(query_message())
                                 , ep
                                 , yield[ec]);
            if (ec && !_cancel) {
                LOG_ERROR("LocalPeerDiscovery: Failed to broadcast search query;"
                          " ec=", ec, " ep=", ep);
            }
        }));
    }

    void start_listening_to_broadcast() {
        auto self = shared_from_this();
        TRACK_SPAWN(_ex, ([this, self] (asio::yield_context yield) mutable {
            sys::error_code ec;
            if (_cancel) return;
            listen_to_broadcast(yield[ec]);
        }));
    }

    void listen_to_broadcast(asio::yield_context yield) {
        string data(256*128, '\0');
        udp::endpoint sender_ep;

        auto cancel_slot = _cancel.connect([&] {
            sys::error_code ec;
            _socket.close(ec);
        });

        while (true) {
            sys::error_code ec;

            size_t size = _socket.async_receive_from( asio::buffer(data)
                                                    , sender_ep
                                                    , yield[ec]);
            if (_cancel) break;

            if (ec) {
                LOG_ERROR("LocalPeerDiscovery: failed to receive;"
                          " ec=", ec);
                async_sleep(_ex, chrono::seconds(1), _cancel, yield);
                if (_cancel) break;
                continue;
            }

            on_broadcast_receive( boost::string_view(data.data(), size)
                                , sender_ep
                                , yield[ec]);

            if (_cancel) break;
            assert(ec != asio::error::operation_aborted);
        }
    }

    string query_message() const {
        ostringstream ss;
        ss << MSG_PREFIX << _id << ":" << MSG_QUERY_CMD;
        for (auto ep :  _advertised_eps) { ss << ep << ";"; }
        return ss.str();
    }

    string reply_message() const {
        ostringstream ss;
        ss << MSG_PREFIX << _id << ":" << MSG_REPLY_CMD;
        for (auto ep :  _advertised_eps) { ss << ep << ";"; }
        return ss.str();
    }

    string bye_message() const {
        ostringstream ss;
        ss << MSG_PREFIX << _id << ":" << MSG_BYE_CMD;
        return ss.str();
    }

    void on_broadcast_receive( boost::string_view sv
                             , udp::endpoint from
                             , asio::yield_context yield)
    {
        if (!consume(sv, MSG_PREFIX)) return;

        auto opt_peer_id = parse::number<decltype(_id)>(sv);

        if (!opt_peer_id || *opt_peer_id == _id) return;
        if (!consume(sv, ":")) return;

        if (consume(sv, MSG_QUERY_CMD)) {
            handle_query(sv, *opt_peer_id, from, yield);
        } else if (consume(sv, MSG_REPLY_CMD)) {
            handle_reply(sv, *opt_peer_id, from);
        } else if (consume(sv, MSG_BYE_CMD)) {
            handle_bye(sv, *opt_peer_id);
        }
    }

    void handle_query( boost::string_view sv
                     , PeerId peer_id
                     , udp::endpoint peer_ep
                     , asio::yield_context yield)
    {
        auto opt_eps = consume_endpoints(sv, peer_ep.address());
        if (!opt_eps) return;
        add_endpoints(peer_id, peer_ep, move(*opt_eps));
        sys::error_code ec;
        _socket.async_send_to( asio::buffer(reply_message())
                             , peer_ep
                             , yield[ec]);
    }

    void handle_reply( boost::string_view sv
                     , PeerId peer_id
                     , udp::endpoint peer_ep)
    {
        auto opt_eps = consume_endpoints(sv, peer_ep.address());
        if (!opt_eps) return;
        add_endpoints(peer_id, peer_ep, move(*opt_eps));
    }

    void handle_bye(boost::string_view sv, PeerId peer_id)
    {
        auto i = _peers.find(peer_id);

        if (i == _peers.end()) return;

        if (logger.would_log(INFO)) {
            ostringstream ss;
            for (auto ep : i->second.advertised_eps) { ss << ep << ";"; }
            LOG_INFO("LocalPeerDiscovery: Lost local ouinet peer(s) ", ss.str());
        }

        _peers.erase(i);
    }

    void add_endpoints(PeerId peer_id, udp::endpoint peer_ep, set<udp::endpoint> eps)
    {
        if (logger.would_log(INFO)) {
            ostringstream ss;
            for (auto ep : eps) { ss << ep << ";"; }
            LOG_INFO("LocalPeerDiscovery: Found local ouinet peer(s) ", ss.str());
        }
        _peers[peer_id] = {peer_ep, move(eps)};
    }
};

set<udp::endpoint> LocalPeerDiscovery::found_peers() const
{
    if (!_impl) return {};

    set<udp::endpoint> ret;

    for (auto& pair : _impl->_peers) {
        auto& eps = pair.second.advertised_eps;
        ret.insert(eps.begin(), eps.end());
    }

    return ret;
}

LocalPeerDiscovery::LocalPeerDiscovery( const AsioExecutor& ex
                                      , set<udp::endpoint> advertised_eps)
    : _ex(ex)
{
    auto id = util::random::number<uint64_t>();
    _impl = make_shared<Impl>(_ex, id, advertised_eps);
    _impl->start();
}

void LocalPeerDiscovery::stop() {
    if (_impl) _impl->_cancel();
}

LocalPeerDiscovery::~LocalPeerDiscovery() {
    if (_impl) _impl->say_bye();
    stop();
}
