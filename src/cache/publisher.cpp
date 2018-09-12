#include "publisher.h"
#include <asio_ipfs.h>
#include <iostream>

#include "../util/crypto.h"
#include "../logger.h"
#include "../bittorrent/dht.h"

using namespace std;
using namespace ouinet;

namespace bt = ouinet::bittorrent;

using boost::optional;
using Timer = asio::steady_timer;
using Clock = chrono::steady_clock;
static const Timer::duration publish_duration = chrono::minutes(10);

struct Publisher::Loop : public enable_shared_from_this<Loop> {
    using PublishFunc = function<void(const string, asio::yield_context)>;

    bool was_stopped = false;
    boost::asio::steady_timer timer;

    std::string to_publish;
    std::string last_value;

    PublishFunc publish_func;

    Loop(asio::io_service& ios) : timer(ios) {}

    void publish(string cid)
    {
        if (cid == last_value) return;

        to_publish = move(cid);
        last_value = to_publish;
        timer.cancel();
    }

    void start()
    {
        asio::spawn(timer.get_io_service(),
                [this, self = shared_from_this()]
                (asio::yield_context yield) {
                    sys::error_code ec;
                    start(yield[ec]);
                });
    }

    void start(asio::yield_context yield)
    {
        auto self = shared_from_this();

        if (was_stopped) return;

        while (true) {
            while (to_publish.empty()) {
                sys::error_code ec;

                timer.expires_from_now(publish_duration / 2);
                timer.async_wait(yield[ec]);

                if (was_stopped) return;

                if (ec && to_publish.empty()) {
                    // Timeout has been reached, force republish the value
                    to_publish = last_value;
                }
            }

            LOG_DEBUG("Publishing DB: ", to_publish);

            auto cid = move(to_publish);

            sys::error_code ec;
            publish_func(cid, yield[ec]);

            if (was_stopped) return;

            LOG_DEBUG("Request was successfully published to cache under id " + cid);
        }
    }

    void stop() {
        was_stopped = true;
        timer.cancel();
    }
};

static
bt::MutableDataItem bt_mutable_data( const string& cid
                                   , const string& ipns
                                   , const util::Ed25519PrivateKey& private_key)
{
    /*
     * Use the sha1 of the URL as salt;
     * Use the timestamp as a version ID.
     */
    using Time = boost::posix_time::ptime;

    Time unix_epoch(boost::gregorian::date(1970, 1, 1));
    Time ts = boost::posix_time::microsec_clock::universal_time();

    string key_hash = util::bytes::to_string(util::sha1(ipns));

    return bt::MutableDataItem::sign( cid
                                    , (ts - unix_epoch).total_milliseconds()
                                    , key_hash
                                    , private_key);
}

static util::Ed25519PrivateKey
decide_private_key( const optional<util::Ed25519PrivateKey>& opt_key_
                  , const fs::path& config_dir)
{
    if (!fs::is_directory(config_dir)) {
        if (fs::exists(config_dir)) {
            cerr << "Error: '" << config_dir << "' is not a directory" << endl;
            exit(1);
        }

        if (!fs::create_directory(config_dir)) {
            cerr << "Error: Failed to create '" << config_dir
                 << "' directory" << endl;
            exit(1);
        }
    }

    fs::path config = config_dir/"bt_publish_private_key";

    if (opt_key_) {
        fs::ofstream(config) << *opt_key_;
        return *opt_key_;
    }

    if (fs::exists(config)) {
        string key_str;
        fs::ifstream(config) >> key_str;

        auto opt_key = util::Ed25519PrivateKey::from_hex(key_str);

        if (!opt_key) {
            cerr << "Failed reading Publisher BT Private key from "
                 << config << endl;

            exit(1);
        }

        return *opt_key;
    }

    auto key = util::Ed25519PrivateKey::generate();

    fs::ofstream(config) << key;

    return key;
}

Publisher::Publisher( asio_ipfs::node& ipfs_node
                    , bt::MainlineDht& bt_dht
                    , const optional<util::Ed25519PrivateKey>& bt_private_key
                    , const fs::path& config_dir)
    : _ios(ipfs_node.get_io_service())
    , _ipfs_node(ipfs_node)
    , _bt_dht(bt_dht)
    , _bt_private_key(decide_private_key(bt_private_key, config_dir))
    , _ipfs_loop(make_shared<Loop>(_ios))
    , _bt_loop(make_shared<Loop>(_ios))
{
    cerr << "Publisher BT Private key: " << _bt_private_key << endl;
    cerr << "Publisher BT Public  key: " << _bt_private_key.public_key() << endl;

    _ipfs_loop->publish_func = [this](auto cid, auto yield) {
        _ipfs_node.publish(cid, publish_duration, yield);
    };

    _bt_loop->publish_func = [this](auto cid, auto yield) {
        auto ipns = _ipfs_node.id();
        auto item = bt_mutable_data(cid, ipns, _bt_private_key);
        _bt_dht.mutable_put_start(item, yield);
    };

    _ipfs_loop->start();
    _bt_loop->start();
}

void Publisher::publish(const std::string& cid)
{
    _ipfs_loop->publish(cid);
    _bt_loop->publish(cid);
}

Publisher::~Publisher()
{
    _ipfs_loop->stop();
    _bt_loop->stop();
}
