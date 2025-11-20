#include <boost/asio/spawn.hpp>

#include "util/wait_condition.h"
#include "bittorrent/mainline_dht.h"
#include "create_udp_multiplexer.h"

using namespace std;
using namespace ouinet;

namespace bt = ouinet::bittorrent;

class BtUtils {
public:
    BtUtils(asio::io_context &ctx)
            : _ctx(ctx)
            , _bt_dht_wc(_ctx)
    {
    }

    std::shared_ptr<bt::MainlineDht> bittorrent_dht(asio::yield_context yield,
                                                    vector<asio::ip::address> ifaddrs)
    {
        if (_bt_dht) return _bt_dht;

        // Ensure that only one coroutine is modifying the instance at a time.
        sys::error_code ec;
        return_or_throw_on_error(yield, _shutdown_signal, ec, _bt_dht);
        if (_bt_dht) return _bt_dht;
        auto lock = _bt_dht_wc.lock();

        auto metrics_client = metrics::Client::noop();
        bool do_doh = true;

        auto bt_dht = std::make_shared<bt::MainlineDht>( _ctx.get_executor(), metrics_client.mainline_dht(), do_doh);
        auto& mpl = common_udp_multiplexer();

        asio_utp::udp_multiplexer m(_ctx);

        m.bind(mpl, ec);
        if (ec) return or_throw(yield, ec, _bt_dht);

        auto cc = _shutdown_signal.connect([&] { bt_dht.reset(); });

        set<asio::ip::udp::endpoint> endpoints;
        for (auto addr : ifaddrs) {
            std::cout << "Spawning DHT node on " << addr << std::endl;
            endpoints.insert({addr, 0});
        }
        bt_dht->set_endpoints(endpoints);

        _bt_dht = std::move(bt_dht);
        return _bt_dht;
    }

    const asio_utp::udp_multiplexer& common_udp_multiplexer()
    {
        if (_udp_multiplexer) return *_udp_multiplexer;

        _udp_multiplexer
                = create_udp_multiplexer( _ctx
                , "/tmp/last_used_udp_port");

        /*
        _udp_reachability
                = make_unique<util::UdpServerReachabilityAnalysis>();
        _udp_reachability->start(get_executor(), *_udp_multiplexer);
        */
        return *_udp_multiplexer;
    }

    void stop() {
        _shutdown_signal();
        if (_bt_dht) {
            _bt_dht->stop();
            _bt_dht = nullptr;
        }
    }

private:
    asio::io_context& _ctx;
    Signal<void()> _shutdown_signal;

    shared_ptr<bt::MainlineDht> _bt_dht;
    WaitCondition _bt_dht_wc;

    boost::optional<asio_utp::udp_multiplexer> _udp_multiplexer;

};
