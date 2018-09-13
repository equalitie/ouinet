#include "resolver.h"
#include "../logger.h"

#include <asio_ipfs.h>

using namespace std;
using namespace ouinet;

struct Resolver::Loop : std::enable_shared_from_this<Loop> {
    bool was_stopped = false;
    string key;
    asio::io_service& ios;
    asio_ipfs::node& ipfs_node;
    asio::steady_timer timer;
    OnResolve on_resolve;

    Loop( const string& key
        , asio_ipfs::node& ipfs_node
        , OnResolve on_resolve
        )
        : key(key)
        , ios(ipfs_node.get_io_service())
        , ipfs_node(ipfs_node)
        , timer(ios)
        , on_resolve(move(on_resolve))
    {}

    void start() {
        asio::spawn(ios, [self = shared_from_this()]
                         (asio::yield_context yield) {
                if (self->was_stopped) return;
                self->run(yield);
            });
    }

    void run(asio::yield_context yield) {
        while (!was_stopped) {
            sys::error_code ec;

            LOG_DEBUG("Resolving IPNS address: " + key);

            auto ipfs_id = ipfs_node.resolve(key, yield[ec]);
            if (was_stopped) return;

            if (!ec) {
                LOG_DEBUG("IPNS ID has been resolved successfully to " + ipfs_id);
                on_resolve(move(ipfs_id), yield[ec]);
                if (was_stopped) return;
            }
            else {
                LOG_ERROR("Error in resolving IPNS: " + ec.message());
            }

            timer.expires_from_now(chrono::seconds(5));
            timer.async_wait(yield[ec]);
        }
    }

    void stop() {
        was_stopped = true;
        timer.cancel();
    }
};

Resolver::Resolver( asio_ipfs::node& ipfs_node
                  , const string& key
                  , OnResolve on_resolve)
    : _loop(make_shared<Loop>(key, ipfs_node, move(on_resolve)))
{
    _loop->start();
}

Resolver::~Resolver()
{
    _loop->stop();
}
