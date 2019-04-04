#include "resolver.h"
#include "../logger.h"
#include "../or_throw.h"
#include "../util/crypto.h"
#include "../bittorrent/dht.h"

#include <asio_ipfs.h>

using namespace std;
using namespace ouinet;
namespace bt = ouinet::bittorrent;

struct Resolver::Loop : std::enable_shared_from_this<Loop> {
    bool was_stopped = false;
    asio::io_service& ios;
    asio::steady_timer timer;
    OnResolve on_resolve;

    Loop( asio::io_service& ios
        , OnResolve on_resolve)
        : ios(ios)
        , timer(ios)
        , on_resolve(move(on_resolve))
    {}

    template<class Resolve>
    void start(Resolve resolve) {
        asio::spawn(ios, [resolve = move(resolve), self = shared_from_this()]
                         (asio::yield_context yield) {
                if (self->was_stopped) return;
                self->run(move(resolve), yield);
            });
    }

    template<class Resolve>
    void run(Resolve resolve, asio::yield_context yield) {
        while (!was_stopped) {
            sys::error_code ec;

            auto ipfs_id = resolve(yield[ec]);

            if (was_stopped) return;

            if (!ec) {
                on_resolve(move(ipfs_id), yield[ec]);
                if (was_stopped) return;
            }

            timer.expires_from_now(chrono::seconds(20));
            timer.async_wait(yield[ec]);
        }
    }

    void stop() {
        was_stopped = true;
        timer.cancel();
    }
};

Resolver::Resolver( asio_ipfs::node& ipfs_node
                  , const string& ipns
                  , bt::MainlineDht& bt_dht
                  , const boost::optional<util::Ed25519PublicKey>& bt_pubkey
                  , OnResolve on_resolve)
    : _ios(ipfs_node.get_io_service())
    , _ipfs_loop(make_shared<Loop>(_ios, on_resolve))
    , _bt_loop(make_shared<Loop>(_ios, on_resolve))
{
    // TODO: It's inefficient to run both of these algorithms
    // concurrently all the time. Perhaps it would be better to
    // chose one and switch to the other one if the first one fails.

    _ipfs_loop->start([ipns, &ipfs_node] (asio::yield_context yield) {
            LOG_DEBUG("Resolving IPNS address: " + ipns + " (IPFS)");  // used by integration tests
            sys::error_code ec;

            auto cid = ipfs_node.resolve(ipns, yield[ec]);

            if (!ec) {
                LOG_DEBUG("IPNS ID has been resolved successfully to "  // used by integration tests
                         + cid + " (IPFS)");
            } else {
                LOG_ERROR("Error in resolving IPNS: "
                         + ec.message() + " (IPFS)");
            }

            return or_throw(yield, ec, cid);
        });

    if (bt_pubkey) {
        _bt_loop->start([ &bt_dht
                        , ipns
                        , pubkey = *bt_pubkey
                        ] (asio::yield_context yield) {
                LOG_DEBUG("Resolving IPNS address: ", ipns + " (BitTorrent)");  // used by integration tests
                sys::error_code ec;

                auto opt_data = bt_dht.mutable_get(pubkey, ipns, yield[ec]);

                if (!ec && !opt_data) {
                    // TODO: This shouldn't happen (it does), the above
                    // function should return an error if not successful.
                    ec = asio::error::not_found;
                }

                string value;

                if (opt_data) {
                    assert(opt_data->verify());

                    if (opt_data->verify()) {
                        auto opt_value = opt_data->value.as_string();
                        if (opt_value) value = *opt_value;
                    }
                }

                if (!ec) {
                    LOG_DEBUG("IPNS ID has been resolved successfully to "  // used by integration tests
                             + value + " (BitTorrent)");
                } else {
                    LOG_ERROR("Error in resolving IPNS address: "
                             + ec.message() + " (BitTorrent)");
                }

                return or_throw(yield, ec, value);
            });
    }
}

Resolver::~Resolver()
{
    _ipfs_loop->stop();
    _bt_loop->stop();
}
