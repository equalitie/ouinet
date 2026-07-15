// Temporary, shall be removed once I'm done with this branch
#define SPEED_DEBUG 0

#include <chrono>
#include <cstddef>
#include <expected>
#include <iostream>
#include <random>
#include <set>

#include <boost/asio/error.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/steady_timer.hpp>
#if SPEED_DEBUG
#   include <boost/optional/optional_io.hpp>
#endif
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/rolling_mean.hpp>
#include <boost/accumulators/statistics/rolling_variance.hpp>
#include <boost/accumulators/statistics/rolling_count.hpp>

#include "bencoding.h"
#include "code.h"
#include "collect.h"
#include "debug_ctx.h"
#include "dht_node.h"
#include "mainline_dht.h"
#include "node_contact.h"
#include "proximity_map.h"
#include "udp_multiplexer.h"

#include "cxx/dns.h"
#include "../async_sleep.h"
#include "../defer.h"
#include "../parse/endpoint.h"
#include "../or_throw.h"
#include "../util.h"
#include "../util/address.h"
#include "../util/atomic_file.h"
#include "../util/bytes.h"
#include "../util/compat.h"
#include "../util/condition_variable.h"
#include "../util/debug.h"
#include "../util/select.h"
#include "../util/sign.h"
#include "../util/str.h"
#include "../util/success_condition.h"
#include "../util/file_io.h"
#include "../util/variant.h"
#include "../logger.h"

namespace ouinet {
namespace bittorrent {

using std::make_unique;
using std::vector;
using std::string;
using boost::string_view;
using std::cerr;
using Candidates = std::vector<NodeContact>;
namespace accum = boost::accumulators;
using Clock = std::chrono::steady_clock;
namespace fs = boost::filesystem;

#define DEBUG_SHOW_MESSAGES 0

// Max number of queries that can be handled concurrently. When this number is reached, any incoming
// queries are ignored.
const int MAX_HANDLE_QUERY_CONCURRENCY = 32;

class Stat {
private:
    using AccumSet = accum::accumulator_set< float
                                           , accum::stats< accum::tag::rolling_mean
                                                         , accum::tag::rolling_variance
                                                         , accum::tag::rolling_count>>;

public:
    using Duration = std::chrono::steady_clock::duration;

public:
    Stat()
        : _accum_set(accum::tag::rolling_window::window_size = 10)
    {}

    void add_reply_time(Duration d)
    {
        using namespace std::chrono;
        float seconds = duration_cast<milliseconds>(d).count() / 1000.f;
        _accum_set(seconds);
    }

    Duration max_reply_wait_time() const {
        //// 2 Should cover ~97.6% of all responses
        //// 3 Should cover ~99.9% of all responses
        auto ov = mean_plus_deviation(3);
        if (!ov) return default_max_reply_wait_time();

        return std::min(*ov, default_max_reply_wait_time());
        //return std::min(3**ov/2, default_max_reply_wait_time());
    }

    static
    Duration default_max_reply_wait_time() {
        return std::chrono::seconds(3);
    }

    static
    Duration seconds_to_duration(float secs) {
        using namespace std::chrono;
        return milliseconds(int64_t(secs*1000.f));
    }

private:
    boost::optional<Duration> mean_plus_deviation(float deviation_multiply) const {
        auto count = accum::rolling_count(_accum_set);

        if (count < 5) return boost::none;

        auto mean     = accum::rolling_mean(_accum_set);
        auto variance = accum::rolling_variance(_accum_set);

        if (variance < 0) return boost::none;

        auto deviation = sqrt(variance);

        return seconds_to_duration(mean + deviation_multiply*deviation);
    }

private:
    AccumSet _accum_set;
};

class DhtNode::Stats {
public:
    using Duration = Stat::Duration;

public:

    void add_reply_time(boost::string_view msg_type, Duration d)
    {
        find_or_create(msg_type).add_reply_time(d);
    }

    Duration max_reply_wait_time(const std::string& msg_type)
    {
        return find_or_create(msg_type).max_reply_wait_time();
    }

private:
    Stat& find_or_create(boost::string_view msg_type) {
        auto i = _per_msg_stat.find(msg_type);
        if (i == _per_msg_stat.end()) {
            auto p = _per_msg_stat.insert(std::make_pair( std::string(msg_type)
                                                        , Stat()));
            return p.first->second;
        }
        return i->second;
    }

private:
    std::map<std::string, Stat, std::less<>> _per_msg_stat;
};

static bool read_nodes( bool is_v4
                      , const BencodedMap& response
                      , PeerFilter peer_filter
                      , util::AsyncQueue<NodeContact>& sink
                      , Async yield)
{
    std::vector<NodeContact> nodes;

    if (is_v4) {
        auto i = response.find("nodes");
        if (i != response.end()) {
            auto os = i->second.as_string_view();
            if (os) NodeContact::decode_compact_v4(*os, nodes);
        }
    } else {
        auto i = response.find("nodes6");
        if (i != response.end()) {
            auto os = i->second.as_string_view();
            if (os) NodeContact::decode_compact_v6(*os, nodes);
        }
    }

    // Remove invalid endpoints
    nodes.erase(
        std::remove_if(
            nodes.begin(),
            nodes.end(),
            [peer_filter] (auto& n) { return !peer_filter.is_allowed(n.endpoint); }
        ),
        nodes.end()
    );

    if (nodes.empty()) return false;

    return compat([&](Cancel cancel, asio::yield_context yield) {
        sink.async_push_many(nodes, cancel, yield);
    })(yield).has_value();
}

DhtNode::DhtNode( const AsioExecutor& exec
                , metrics::DhtNode metrics
                , std::shared_ptr<dns::Resolver> dns_resolver
                , const uint32_t mux_rx_limit
                , fs::path storage_dir
                , bootstrap::Config bs
                , util::LogPath log_path
):
    _exec(exec),
    _ready(false),
    _stats(new Stats()),
    _dns_resolver(std::move(dns_resolver)),
    _mux_rx_limit(mux_rx_limit),
    _storage_dir(std::move(storage_dir)),
    _bootstrap_config(std::move(bs)),
    _metrics(std::move(metrics)),
    _log_path(std::move(log_path))
{
}

std::expected<void, sys::error_code> DhtNode::start(udp::endpoint local_ep, Async yield)
{
    if (local_ep.address().is_loopback()) {
        LOG_WARN(yield, " Node shall be bound to the loopback address and "
                      , "thus won't be able to communicate with the world");
    }

    auto m = asio_utp::udp_multiplexer(_exec);

    sys::error_code ec;
    m.bind(local_ep, ec);

    if (ec) {
        return std::unexpected(ec);
    }

    return start(std::move(m), yield);
}

std::expected<void, sys::error_code> DhtNode::start(asio_utp::udp_multiplexer m, Async yield)
{
    _multiplexer = std::make_unique<UdpMultiplexer>(std::move(m), _mux_rx_limit);

    _tracker = std::make_unique<Tracker>(_exec);
    _data_store = std::make_unique<DataStore>(_exec);

    _node_id = NodeID::zero();
    _next_transaction_id = 1;

    yield.spawn(_cancel, [this] (auto yield) {
        receive_loop(yield);
    });

    auto result = bootstrap(yield);
    if (!result) {
        return std::unexpected(result.error());
    }

    yield.spawn(_cancel, [this] (auto yield) {
        store_contacts_loop(yield);
    });

    return {};
}

fs::path DhtNode::stored_contacts_path() const
{
    if (_storage_dir == fs::path()) return fs::path();
    string ipv = _local_endpoint.address().is_v4() ? "ipv4" : "ipv6";
    return _storage_dir / util::str("stored_peers-", ipv, ".txt");
}

static
std::expected<std::set<NodeContact>, sys::error_code>
read_stored_contacts(const fs::path& path, Async yield)
{
    std::set<NodeContact> ret;

    auto file = util::file_io::open_readonly(yield.get_executor(), path);
    if (!file) {
        return std::unexpected(file.error());
    }

    auto filesize = util::file_io::file_size(*file);
    if (!filesize) {
        return std::unexpected(filesize.error());
    }

    std::string data(*filesize, '\0');

    auto result = util::file_io::read(*file, asio::buffer(data), yield);

    if (!result) {
        return std::unexpected(result.error());
    }

    boost::string_view sw = data;

    while (!sw.empty()) {
        auto pos = sw.find('\n');
        auto s = sw.substr(0, pos);

        if (pos == sw.npos) sw = sw.substr(sw.size(), 0);
        else                sw = sw.substr(pos + 1);

        auto comma_pos = s.find(',');

        if (comma_pos == s.npos || comma_pos == s.npos - 1) continue;

        auto id_s = s.substr(0, comma_pos);
        auto ep_s = s.substr(comma_pos+1);

        auto opt_id = NodeID::from_hex(id_s);
        auto opt_ep = parse::endpoint<udp>(ep_s);

        if (!opt_ep || !opt_id) continue;

        ret.insert({*opt_id, *opt_ep});
    }

    return ret;
}

static
std::expected<void, sys::error_code>
write_stored_contacts( std::set<NodeContact> contacts
                     , const fs::path& path
                     , Async yield)
{
    auto old_contacts = read_stored_contacts(path, yield);
    if (!old_contacts) {
        return std::unexpected(old_contacts.error());
    }

    auto result0 = util::file_io::check_or_create_directory(path.parent_path());
    if (!result0) {
        LOG_ERROR(yield, " Failed to store contacts: ", result0.error());
        return std::unexpected(result0.error());
    }

    auto atomic_file = util::atomic_file::make(yield.get_executor(), path);
    if (!atomic_file) {
        LOG_ERROR(yield, " Failed to store contacts: ", atomic_file.error());
        return std::unexpected(atomic_file.error());
    }

    string data;

    for (unsigned i = 0; i < 500; ++i) {
        NodeContact c;

        if (!contacts.empty()) {
            auto iter = contacts.begin();
            c = *iter;
            contacts.erase(iter);
        } else if (!old_contacts->empty()) {
            auto iter = old_contacts->begin();
            c = *iter;
            old_contacts->erase(iter);
        } else {
            break;
        }

        if (i != 0) data += '\n';
        data += util::str(c.id, ",", c.endpoint);
    }

    auto result1 = util::file_io::write(
            atomic_file->lowest_layer(),
            asio::buffer(data),
            yield
        );

    if (!result1) {
        LOG_ERROR(yield, " Failed to store contacts: ", result1.error());
        return std::unexpected(result1.error());
    }

    auto result2 = compat([&](sys::error_code& ec) {
        atomic_file->commit(ec);
    })();
    if (!result2) {
        LOG_ERROR(yield, " Failed to store contacts: ", result2.error());
        return std::unexpected(result2.error());
    }

    LOG_DEBUG(yield, " Successfully stored contacts");

    return {};
}

void DhtNode::store_contacts() const
{
    if (!_routing_table) return;

    fs::path path = stored_contacts_path();

    if (path == fs::path()) return;

    auto contacts = _routing_table->dump_contacts();

    task::spawn_detached(_exec, ([
        path = std::move(path),
        contacts = std::move(contacts),
        cancel = _cancel,
        log_path = _log_path
    ] (asio::yield_context yield) mutable {
        std::ignore = write_stored_contacts(
            std::move(contacts),
            path,
            Async(yield, std::move(cancel), std::move(log_path))
        );
    }));
}

void DhtNode::stop()
{
    store_contacts();

    _multiplexer = nullptr;
    _tracker = nullptr;
    _data_store = nullptr;
    _cancel();
}

DhtNode::~DhtNode()
{
    stop();
}

std::expected<std::set<udp::endpoint>, sys::error_code> DhtNode::tracker_get_peers(
    NodeID infohash,
    Async yield
) {
    std::set<udp::endpoint> peers;
    std::map<NodeID, TrackerNode> responsible_nodes;

    auto result = tracker_do_search_peers(infohash, peers, responsible_nodes, yield);

    if (!result) {
        return std::unexpected(result.error());
    }

    return peers;
}

std::expected<std::set<udp::endpoint>, sys::error_code> DhtNode::tracker_announce(
    NodeID infohash,
    std::optional<int> port,
    Async yield
) {
    std::set<udp::endpoint> peers;
    std::map<NodeID, TrackerNode> responsible_nodes;

    auto result = tracker_do_search_peers(infohash, peers, responsible_nodes, yield);
    if (!result) {
        return std::unexpected(result.error());
    }

    bool success = false;
    WaitCondition wc(_exec);
    for (auto& i : responsible_nodes) {
        yield.spawn([&, i, lock = wc.lock()] (Async yield) {
            auto result = send_write_query(
                i.second.node_endpoint,
                i.first,
                "announce_peer",
                BencodedMap {
                    { "id", _node_id.to_bytestring() },
                    { "info_hash", infohash.to_bytestring() },
                    { "token", i.second.announce_token },
                    { "implied_port", port ? int64_t(0) : int64_t(1) },
                    { "port", port ? int64_t(*port) : int64_t(0) }
                },
                yield
            );

            if (result) {
                success = true;
            } else {
                LOG_WARN(yield, " failed to send announce_peer query to ", i.second.node_endpoint, ": ", result.error());
            }
        });
    }
    wc.wait(yield);

    if (success) {
        return std::move(peers);
    } else {
        return std::unexpected(boost::asio::error::network_down);
    }
}

std::optional<BencodedValue> DhtNode::data_get_immutable(const NodeID& key, Async yield) {
    /*
     * This is a ProximitySet, really.
     */
    ProximityMap<boost::none_t> responsible_nodes(key, RESPONSIBLE_TRACKERS_PER_SWARM);
    std::optional<BencodedValue> data;

    DebugCtx dbg;
    dbg.enable_log = SPEED_DEBUG;

    auto result = collect(dbg, key, [&](
        const Contact& candidate,
        WatchDog& wd,
        util::AsyncQueue<NodeContact>& closer_nodes,
        Async yield
    ) {
        if (!candidate.id && responsible_nodes.full()) {
            return;
        }

        if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
            return;
        }

        /*
         * As soon as we have found a valid data value, we can stop the search.
         */
        if (data) {
            return;
        }

        auto response_ = compat([&](Cancel cancel, asio::yield_context yield) {
            return query_get_data(
                key,
                candidate,
                closer_nodes,
                wd, &dbg,
                cancel,
                yield
            );
        })(yield);
        if (!response_) return;
        if (!*response_) return;

        BencodedMap& response = **response_;

        if (candidate.id) {
            responsible_nodes.insert({ *candidate.id, boost::none });
        }

        if (response.count("v")) {
            BencodedValue value = response["v"];
            if (DataStore::immutable_get_id(value) == key) {
                data = value;
                return;
            }
        }
    }, yield);

    if (!result) {
        return std::nullopt;
    }

    return data;
}

NodeID DhtNode::data_put_immutable(
    const BencodedValue& data,
    Cancel& cancel,
    asio::yield_context yield
) {
    NodeID key = DataStore::immutable_get_id(data);

    sys::error_code ec;
    struct ResponsibleNode {
        asio::ip::udp::endpoint node_endpoint;
        std::string put_token;
    };
    ProximityMap<ResponsibleNode> responsible_nodes(key, RESPONSIBLE_TRACKERS_PER_SWARM);

    DebugCtx dbg;
    dbg.enable_log = SPEED_DEBUG;

    compat([&](Async yield) {
        return collect(dbg, key, [&](
            const Contact& candidate,
            WatchDog& wd,
            util::AsyncQueue<NodeContact>& closer_nodes,
            Async yield
        ) {
            if (!candidate.id && responsible_nodes.full()) {
                return;
            }

            if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
                return;
            }

            auto response_ = compat([&](Cancel cancel, asio::yield_context yield) {
                return query_get_data(
                    key,
                    candidate,
                    closer_nodes,
                    wd, &dbg,
                    cancel,
                    yield
                );
            })(yield);

            if (!response_) return;
            if (!*response_) return;

            BencodedMap& response = **response_;

            boost::optional<std::string> put_token = response["token"].as_string();

            if (!put_token) return;

            if (candidate.id) {
                responsible_nodes.insert({
                    *candidate.id,
                    { candidate.endpoint, std::move(*put_token) }
                });
            }
        }, yield);
    })(cancel, yield[ec]);

    if (ec) {
        return or_throw<NodeID>(yield, ec, std::move(key));
    }

    bool success = false;
    auto cancelled = cancel.connect([]{});
    WaitCondition wc(_exec);
    for (auto& i : responsible_nodes) {
        task::spawn_detached(_exec, [&, lock = wc.lock()] (asio::yield_context yield) {
            sys::error_code ec;

            compat([&](Async yield) {
                return send_write_query(
                    i.second.node_endpoint,
                    i.first,
                    "put",
                    {
                        { "id", _node_id.to_bytestring() },
                        { "v", data },
                        { "token", i.second.put_token }
                    },
                    yield
                );
            })(cancel, yield[ec]);
            if (!ec) {
                success = true;
            }
        });
    }
    wc.wait(yield);

    ec = cancelled ? boost::asio::error::operation_aborted : success ? sys::error_code() : boost::asio::error::network_down;

    return or_throw<NodeID>(yield, ec, std::move(key));
}

boost::optional<MutableDataItem> DhtNode::data_get_mutable(
    const sign::PublicKey& public_key,
    boost::string_view salt,
    Cancel& cancel,
    asio::yield_context yield
) {
    NodeID target_id = DataStore::mutable_get_id(public_key, salt);

    sys::error_code ec;
    /*
     * This is a ProximitySet, really.
     */
    ProximityMap<boost::none_t> responsible_nodes(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);
    boost::optional<MutableDataItem> data;

    Cancel internal_cancel(cancel);

    boost::optional<WatchDog> cancel_wd;

    DebugCtx dbg;
    dbg.enable_log = SPEED_DEBUG;

    compat([&](Async yield) {
        return collect(dbg, target_id, [&](
            const Contact& candidate,
            WatchDog& wd,
            util::AsyncQueue<NodeContact>& closer_nodes,
            Async yield
        ) {
            if (!candidate.id && responsible_nodes.full()) {
                return;
            }

            if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
                return;
            }

            /*
            * We want to find the latest version of the data, so don't stop early.
            */

            auto response_ = compat([&](Cancel cancel, asio::yield_context yield) {
                return query_get_data2(
                    target_id,
                    candidate,
                    closer_nodes,
                    wd,
                    dbg,
                    cancel,
                    yield
                );
            })(yield);

            if (!response_) return;
            if (!*response_) return;

            BencodedMap& response = **response_;

            if (candidate.id) {
                responsible_nodes.insert({ *candidate.id, boost::none });
            }

            if (response["k"] != util::bytes::to_string(public_key.to_bytes())) {
                return;
            }

            boost::optional<int64_t> sequence_number = response["seq"].as_int();
            if (!sequence_number) return;

            auto signature = response["sig"].as_string_view();
            if (!signature || signature->size() != sign::Signature::size) return;

            MutableDataItem item {
                public_key,
                std::string(salt),
                response["v"],
                *sequence_number,
                { util::bytes::to_array<uint8_t, sign::Signature::size>(*signature) }
            };
            if (item.verify()) {
                if (!data || *sequence_number > data->sequence_number) {
                    data = std::move(item);
                    /*
                    * XXX: This isn't correct! We shouldn't stop with the first
                    * validly signed item we get. Ideally we would get the item
                    * from some N closest nodes to `target_id`. But that is
                    * impractical because many of the closest nodes won't respond
                    * and make us wait for too long (and we sometimes time-out even
                    * though there _is_ some value already).
                    *
                    * TODO: Make this function not return a single value, but a
                    * "generator" of dht mutable items. Then the user of this
                    * function can have a look at it and decide whether it's
                    * "fresh enough" (e.g. if it's a http response, it may still
                    * be fresh).
                    */
                    if (!cancel_wd) {
                        cancel_wd = WatchDog(_exec
                                            , std::chrono::seconds(5)
                                            , [&] { cancel(); });
                    }
                }
            }
        }, yield);
    })(internal_cancel, yield[ec]);

    if (ec == asio::error::operation_aborted && !cancel && data) {
        // Only internal cancel was called to indicate we're done
        ec = sys::error_code();
    }

    return or_throw(yield, ec, std::move(data));
}

NodeID DhtNode::data_put_mutable(
    MutableDataItem data,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    Cancel local_cancel(cancel_signal);

    NodeID target_id = DataStore::mutable_get_id(data.public_key, data.salt);

    sys::error_code ec;
    ProximityMap<boost::none_t> responsible_nodes(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);

    using namespace std::chrono;

    DebugCtx dbg;

    auto write_to_node = [&] ( const NodeID& id
                             , udp::endpoint ep
                             , const string_view put_token
                             , WatchDog& wd
                             , Cancel& cancel
                             , asio::yield_context yield) -> bool {
            BencodedMap put_message {
                { "id", _node_id.to_bytestring() },
                { "k", util::bytes::to_string(data.public_key.to_bytes()) },
                { "seq", data.sequence_number },
                { "sig", util::bytes::to_string(data.signature.bytes) },
                { "v", data.value },
                { "token", std::string(put_token) }
            };

            if (!data.salt.empty()) {
                put_message["salt"] = data.salt;
            }

            wd.expires_after(_stats->max_reply_wait_time("put"));

            sys::error_code ec;
            compat([&](Async yield) {
                return send_write_query(ep, id, "put", put_message, yield);
            })(cancel, yield[ec]);

            if (cancel) ec = asio::error::operation_aborted;

            return !ec ? true : false;
    };

    std::set<udp::endpoint> blacklist;

    compat([&](Async yield) {
        return collect(dbg, target_id, [&](
            const Contact& candidate,
            WatchDog& wd,
            util::AsyncQueue<NodeContact>& closer_nodes,
            Async yield
        ) {
            if (!candidate.id && responsible_nodes.full()) {
                return;
            }

            if (candidate.id && !responsible_nodes.would_insert(*candidate.id)) {
                return;
            }

            if (blacklist.count(candidate.endpoint)) {
                return;
            }

            auto response_ = compat([&](Cancel cancel, asio::yield_context yield) {
                return query_get_data3(
                    target_id,
                    candidate,
                    closer_nodes,
                    wd,
                    dbg,
                    cancel,
                    yield
                );
            })(yield);

            if (!response_ || !*response_) {
                blacklist.insert(candidate.endpoint);
                return;
            }

            BencodedMap& response = **response_;

            auto put_token = response["token"].as_string_view();
            if (!put_token) {
                return;
            }

            if (candidate.id) {
                if (responsible_nodes.would_insert(*candidate.id)) {
                    auto write_success = compat([&](Cancel cancel, asio::yield_context yield) {
                        return write_to_node( *candidate.id
                                            , candidate.endpoint
                                            , *put_token
                                            , wd
                                            , cancel
                                            , yield);
                    })(yield);

                    if (!write_success || !*write_success) {
                        responsible_nodes.insert({*candidate.id, boost::none});
                        return;
                    }
                }
            }

            if (response["k"] != util::bytes::to_string(data.public_key.to_bytes())) {
                return;
            }

            boost::optional<int64_t> response_seq = response["seq"].as_int();
            if (!response_seq) return;

            auto response_sig = response["sig"].as_string_view();
            if (!response_sig || response_sig->size() != sign::Signature::size) return;

            MutableDataItem item {
                data.public_key,
                data.salt,
                response["v"],
                *response_seq,
                { util::bytes::to_array<uint8_t, sign::Signature::size>(*response_sig) }
            };

            if (item.verify()) {
                if (*response_seq < data.sequence_number) {
                    /*
                    * This node has an old version of this data entry.
                    * Update it even if it is no longer responsible.
                    */
                    std::ignore = compat([&](Cancel cancel, asio::yield_context yield) {
                        return write_to_node( *candidate.id
                                            , candidate.endpoint
                                            , *put_token
                                            , wd
                                            , cancel
                                            , yield);
                    })(yield);
                }
            }
        }, yield);
    })(local_cancel, yield[ec]);

    if (cancel_signal) {
        ec = asio::error::operation_aborted;
    } else if (responsible_nodes.empty()) {
        ec = asio::error::network_down;
    }

    return or_throw<NodeID>(yield, ec, std::move(target_id));
}


void DhtNode::receive_loop(Async yield)
{
    bool handle_query_ok = true;
    int concurrency = 0;

    while (handle_query_ok) {
        /*
         * Later versions of boost::asio make it possible to (1) wait for a
         * datagram, (2) find out the size, (3) allocate a buffer, (4) recv
         * the datagram. Unfortunately, boost::asio 1.62 does not support that.
         */
        udp::endpoint sender;

        auto packet = compat([&](Cancel cancel, asio::yield_context yield) {
            return _multiplexer->receive(sender, cancel, yield);
        })(yield);
        if (!packet) {
            break;
        }

        // TODO: The bencode parser should only need a string_view.
        boost::optional<BencodedValue> decoded_message = bencoding_decode(*packet);

        if (!decoded_message) {
#           if DEBUG_SHOW_MESSAGES
            LOG_DEBUG(yield, " recv: ", sender, " Failed parsing \"", packet, "\"");
#           endif

            continue;
        }

#       if DEBUG_SHOW_MESSAGES
        LOG_DEBUG(yield, " recv: ", sender, " ", *decoded_message);
#       endif

        BencodedMap* message_map = decoded_message->as_map();
        if (!message_map) {
            continue;
        }

        if (!message_map->count("y") || !message_map->count("t")) {
            continue;
        }

        boost::optional<string_view> message_type = (*message_map)["y"].as_string_view();
        boost::optional<string_view> transaction_id = (*message_map)["t"].as_string_view();
        if (!message_type || !transaction_id) {
            continue;
        }

        if (*message_type == "q") {
            if (concurrency >= MAX_HANDLE_QUERY_CONCURRENCY) {
                LOG_WARN(yield, " too many concurrent queries");
                continue;
            }

            ++concurrency;

            yield.spawn(
                [
                    this,
                    sender,
                    message_map = std::move(*message_map),
                    &handle_query_ok,
                    &concurrency
                ] (Async yield) mutable {
                    auto cleanup = defer([&] {
                        --concurrency;
                    });

                    auto result = handle_query(sender, message_map, yield);
                    if (!result) {
                        handle_query_ok = false;
                    }
                }
            );
        } else if (*message_type == "r" || *message_type == "e") {
            auto it = _active_requests.find(*transaction_id);
            if (it != _active_requests.end() && it->second.destination == sender) {
                it->second.callback(std::move(*message_map));
            }
        }
    }
}

void DhtNode::store_contacts_loop(Async yield)
{
    fs::path path = stored_contacts_path();
    if (path == fs::path()) return;

    while (true) {
        if (!_routing_table) return;
        auto contacts = _routing_table->dump_contacts();

        std::ignore = write_stored_contacts(std::move(contacts), path, yield);

        async_sleep(std::chrono::minutes(6), yield);
    }
}

std::string DhtNode::new_transaction_string()
{
#if 0 // Useful for debugging
    std::ostringstream ss;
    ss << _next_transaction_id++;
    return ss.str();
#else
    uint32_t transaction_id = _next_transaction_id++;

    if (transaction_id == 0) {
        return std::string(1 /* count */, '\0');
    }

    std::string ret;

    while (transaction_id) {
        unsigned char c = transaction_id & 0xff;
        transaction_id = transaction_id >> 8;
        ret += c;
    }

    return ret;
#endif
}

std::expected<void, sys::error_code> DhtNode::send_datagram(
    udp::endpoint destination,
    const BencodedMap& message,
    Async yield
) {
#   if DEBUG_SHOW_MESSAGES
    LOG_DEBUG(yield, " send: ", destination, " ", message, " :: ", i->second);
#   endif
    return compat([&](Cancel cancel, asio::yield_context yield) {
        _multiplexer->send(
            bencoding_encode(message),
            destination,
            cancel,
            yield
        );
    })(yield);
}

std::expected<void, sys::error_code> DhtNode::send_query(
    udp::endpoint destination,
    std::string transaction,
    std::string query_type,
    BencodedMap query_arguments,
    Async yield
) {
    return send_datagram(
        destination,
        BencodedMap {
            { "y", "q" },
            { "q", std::move(query_type) },
            { "a", std::move(query_arguments) },
            // TODO: version string
            { "t", std::move(transaction) }
        },
        yield
    );
}

/*
 * Send a query message to a destination, and wait for either a reply, an error
 * reply, or a timeout.
 *
 * If destination_id is set, update the routing table in accordance with
 * whether a successful reply was received.
 */
std::expected<BencodedMap, sys::error_code> DhtNode::send_query_await_reply(
    Contact dst,
    const std::string& query_type,
    const BencodedMap& query_arguments,
    WatchDog* dms,
    DebugCtx* dbg,
    Async yield
) {
    using namespace std::chrono;

    auto cancel_con = _cancel.connect([&]() { yield.cancel(); });

    //auto timeout_duration = _stats->max_reply_wait_time(query_type);
    auto timeout_duration = std::chrono::seconds(10);

    if (dms) {
        auto d1 = dms->time_to_finish();
        auto d2 = _stats->max_reply_wait_time(query_type);

        dms->expires_after(std::max(d1,d2));
    }

    auto start = Clock::now();

    std::string transaction = new_transaction_string();

    std::optional<BencodedMap> response;
    ConditionVariable response_cv(yield.get_executor());

    _active_requests[transaction] = {
        dst.endpoint,
        [&] (BencodedMap&& response_) {
            response = std::move(response_);
            response_cv.notify();
        }
    };

    auto cleanup = defer([&, cancel = _cancel]() {
        if (!cancel) {
            _active_requests.erase(transaction);
        }
    });

    auto result = timeout(
        timeout_duration,
        [&](auto yield) -> std::expected<BencodedMap, sys::error_code> {
            auto send_result = send_query(
                dst.endpoint,
                transaction,
                std::move(query_type),
                std::move(query_arguments),
                yield
            );

            if (!send_result) {
                return std::unexpected(send_result.error());
            }

            while (!response) {
                response_cv.wait(yield);
            }

            return std::move(*response);
        },
        yield
    );

    if (result) {
        _stats->add_reply_time(query_type, Clock::now() - start);
    }

    if (dst.id) {
        NodeContact contact{ .id = *dst.id, .endpoint = dst.endpoint };

        if (!result || (*result)["y"] != "r") {
            // Record the failure in the routing table.
            _routing_table->fail_node(contact);
        } else {
            // Add the node to the routing table, subject to space limitations.
            _routing_table->try_add_node(contact, true);
        }
    }

    return result;
}

std::expected<void, sys::error_code> DhtNode::handle_query(udp::endpoint sender, BencodedMap& query, Async yield)
{
    assert(query["y"] == "q");

    const auto transaction_ = query["t"].as_string_view();

    if (!transaction_) { return {}; }

    const auto transaction = *transaction_;

    auto send_error = [&] (int code, std::string description, Async yield) {
        return send_datagram(
            sender,
            BencodedMap {
                { "y", "e" },
                { "t", std::string(transaction) },
                { "e", BencodedList{code, description} },
                { "ip", encode_endpoint(sender) }
            },
            yield
        );
    };

    auto send_reply = [&] (BencodedMap reply, Async yield) {
        reply["id"] = _node_id.to_bytestring();

        return send_datagram(
            sender,
            BencodedMap {
                // TODO: Send version "v" (same in
                // above error reply).
                // https://wiki.theory.org/BitTorrentSpecification
                // http://www.bittorrent.org/beps/bep_0020.html
                { "y", "r" },
                { "t", std::string(transaction) },
                { "r", std::move(reply) },
                { "ip", encode_endpoint(sender) },
            },
            yield
        );
    };

    if (!query["q"].is_string()) {
        return send_error(203, "Missing field 'q'", yield);
    }
    string_view query_type = *query["q"].as_string_view();

    if (!query["a"].is_map()) {
        return send_error(203, "Missing field 'a'", yield);
    }
    BencodedMap& arguments = *query["a"].as_map();

    boost::optional<string_view> sender_id = arguments["id"].as_string_view();
    if (!sender_id) {
        return send_error(203, "Missing argument 'id'", yield);
    }
    if (sender_id->size() != 20) {
        return send_error(203, "Malformed argument 'id'", yield);
    }
    NodeContact contact;
    contact.id = NodeID::from_bytestring(*sender_id);
    contact.endpoint = sender;

    /*
     * Per BEP 43, if the query contains a read-only flag, do not consider the
     * sender for any routing purposes.
     */
    boost::optional<int64_t> read_only_flag = arguments["ro"].as_int();
    if (_routing_table && (!read_only_flag || *read_only_flag != 1)) {
        /*
        * Add the sender to the routing table.
        */
        _routing_table->try_add_node(contact, false);
    }

    if (query_type == "ping") {
        return send_reply({}, yield);
    } else if (query_type == "find_node") {
        boost::optional<string_view> target_id_ = arguments["target"].as_string_view();
        if (!target_id_) {
            return send_error(203, "Missing argument 'target'", yield);
        }
        if (target_id_->size() != 20) {
            return send_error(203, "Malformed argument 'target'", yield);
        }
        NodeID target_id = NodeID::from_bytestring(*target_id_);

        BencodedMap reply;

        std::vector<NodeContact> contacts;

        if (_routing_table) {
            contacts = _routing_table->find_closest_routing_nodes(target_id, RoutingTable::BUCKET_SIZE);
        }

        std::string nodes;
        if (!contacts.empty() && contacts[0].id == target_id) {
            nodes += contacts[0].id.to_bytestring();
            nodes += encode_endpoint(contacts[0].endpoint);
        } else {
            for (auto& contact : contacts) {
                nodes += contact.id.to_bytestring();
                nodes += encode_endpoint(contact.endpoint);
            }
        }
        if (is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }

        return send_reply(reply, yield);
    } else if (query_type == "get_peers") {
        boost::optional<string_view> infohash_ = arguments["info_hash"].as_string_view();
        if (!infohash_) {
            return send_error(203, "Missing argument 'info_hash'", yield);
        }
        if (infohash_->size() != 20) {
            return send_error(203, "Malformed argument 'info_hash'", yield);
        }
        NodeID infohash = NodeID::from_bytestring(*infohash_);

        BencodedMap reply;

        std::vector<NodeContact> contacts;

        if (_routing_table) {
            contacts = _routing_table->find_closest_routing_nodes(infohash, RoutingTable::BUCKET_SIZE);
        }

        std::string nodes;
        for (auto& contact : contacts) {
            nodes += contact.id.to_bytestring();
            nodes += encode_endpoint(contact.endpoint);
        }
        if (is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }

        reply["token"] = _tracker->generate_token(sender.address(), infohash);

        /*
         * 50 peers will comfortably fit in a single UDP packet even in the
         * worst case.
         */
        const int NUM_PEERS = 50;
        std::vector<tcp::endpoint> peers = _tracker->list_peers(infohash, NUM_PEERS);
        if (!peers.empty()) {
            BencodedList peer_list;
            for (auto& peer : peers) {
                peer_list.push_back(encode_endpoint(peer));
            }
            reply["values"] = peer_list;
        }

        return send_reply(reply, yield);
    } else if (query_type == "announce_peer") {
        boost::optional<string_view> infohash_ = arguments["info_hash"].as_string_view();
        if (!infohash_) {
            return send_error(203, "Missing argument 'info_hash'", yield);
        }
        if (infohash_->size() != 20) {
            return send_error(203, "Malformed argument 'info_hash'", yield);
        }
        NodeID infohash = NodeID::from_bytestring(*infohash_);

        boost::optional<string_view> token_ = arguments["token"].as_string_view();
        if (!token_) {
            return send_error(203, "Missing argument 'token'", yield);
        }
        string_view token = *token_;
        boost::optional<int64_t> port_ = arguments["port"].as_int();
        if (!port_) {
            return send_error(203, "Missing argument 'port'", yield);
        }
        boost::optional<int64_t> implied_port_ = arguments["implied_port"].as_int();
        int effective_port;
        if (implied_port_ && *implied_port_ == 1) {
            effective_port = sender.port();
        } else {
            effective_port = *port_;
        }

        /*
         * Reject announce_peer requests for which there are more than enough
         * better responsible known nodes.
         *
         * TODO: This can be done in a more efficient way once the routing
         * table code stabilizes.
         */
        if (_routing_table) {
            bool contains_self = false;
            std::vector<NodeContact> closer_nodes = _routing_table->find_closest_routing_nodes(infohash, RESPONSIBLE_TRACKERS_PER_SWARM * 4);
            for (auto& i : closer_nodes) {
                if (infohash.closer_to(_node_id, i.id)) {
                    contains_self = true;
                }
            }
            if (!contains_self) {
                return send_error(201, "This torrent is not my responsibility", yield);
            }
        }

        if (!_tracker->verify_token(sender.address(), infohash, token)) {
            return send_error(203, "Incorrect announce token", yield);
        }

        _tracker->add_peer(infohash, tcp::endpoint(sender.address(), effective_port));

        return send_reply({}, yield);
    } else if (query_type == "get") {
        boost::optional<string_view> target_ = arguments["target"].as_string_view();
        if (!target_) {
            return send_error(203, "Missing argument 'target'", yield);
        }
        if (target_->size() != 20) {
            return send_error(203, "Malformed argument 'target'", yield);
        }
        NodeID target = NodeID::from_bytestring(*target_);

        boost::optional<int64_t> sequence_number_ = arguments["seq"].as_int();

        BencodedMap reply;

        std::vector<NodeContact> contacts
            = _routing_table->find_closest_routing_nodes(target, RoutingTable::BUCKET_SIZE);
        std::string nodes;
        for (auto& contact : contacts) {
            nodes += contact.id.to_bytestring();
            nodes += encode_endpoint(contact.endpoint);
        }
        if (is_v4()) {
            reply["nodes"] = nodes;
        } else {
            reply["nodes6"] = nodes;
        }

        reply["token"] = _data_store->generate_token(sender.address(), target);

        if (!sequence_number_) {
            boost::optional<BencodedValue> immutable_value = _data_store->get_immutable(target);
            if (immutable_value) {
                reply["v"] = *immutable_value;
                return send_reply(reply, yield);
            }
        }

        boost::optional<MutableDataItem> mutable_item = _data_store->get_mutable(target);
        if (mutable_item) {
            if (sequence_number_ && *sequence_number_ <= mutable_item->sequence_number) {
                return send_reply(reply, yield);
            }

            reply["k"] = util::bytes::to_string(mutable_item->public_key.to_bytes());
            reply["seq"] = mutable_item->sequence_number;
            reply["sig"] = util::bytes::to_string(mutable_item->signature.bytes);
            reply["v"] = mutable_item->value;
            return send_reply(reply, yield);
        }

        return send_reply(reply, yield);
    } else if (query_type == "put") {
        boost::optional<string_view> token_ = arguments["token"].as_string_view();
        if (!token_) {
            return send_error(203, "Missing argument 'token'", yield);
        }

        if (!arguments.count("v")) {
            return send_error(203, "Missing argument 'v'", yield);
        }
        BencodedValue value = arguments["v"];
        /*
         * Size limit specified in BEP 44
         */
        if (bencoding_encode(value).size() >= 1000) {
            return send_error(205, "Argument 'v' too big", yield);
        }

        if (arguments["k"].is_string()) {
            /*
             * This is a mutable data item.
             */
            boost::optional<string_view> public_key_ = arguments["k"].as_string_view();
            if (!public_key_) {
                return send_error(203, "Missing argument 'k'", yield);
            }
            if (public_key_->size() != sign::PublicKey::size) {
                return send_error(203, "Malformed argument 'k'", yield);
            }
            sign::PublicKey public_key(util::bytes::to_array<uint8_t, sign::PublicKey::size>(*public_key_));

            boost::optional<string_view> signature_ = arguments["sig"].as_string_view();
            if (!signature_) {
                return send_error(203, "Missing argument 'sig'", yield);
            }
            if (signature_->size() != sign::Signature::size) {
                return send_error(203, "Malformed argument 'sig'", yield);
            }
            sign::Signature::Bytes signature = util::bytes::to_array<uint8_t, sign::Signature::size>(*signature_);

            boost::optional<int64_t> sequence_number_ = arguments["seq"].as_int();
            if (!sequence_number_) {
                return send_error(203, "Missing argument 'seq'", yield);
            }
            int64_t sequence_number = *sequence_number_;

            boost::optional<string> salt_ = arguments["salt"].as_string();
            /*
             * Size limit specified in BEP 44
             */
            if (salt_ && salt_->size() > 64) {
                return send_error(207, "Argument 'salt' too big", yield);
            }
            std::string salt = salt_ ? std::move(*salt_) : "";

            NodeID target = _data_store->mutable_get_id(public_key, salt);

            if (!_data_store->verify_token(sender.address(), target, *token_)) {
                return send_error(203, "Incorrect put token", yield);
            }

            /*
             * Reject put requests for which there are more than enough
             * better responsible known nodes.
             *
             * TODO: This can be done in a more efficient way once the routing
             * table code stabilizes.
             */
            {
                bool contains_self = false;
                std::vector<NodeContact> closer_nodes = _routing_table->find_closest_routing_nodes(target, RESPONSIBLE_TRACKERS_PER_SWARM * 4);
                for (auto& i : closer_nodes) {
                    if (target.closer_to(_node_id, i.id)) {
                        contains_self = true;
                    }
                }
                if (!contains_self) {
                    return send_error(201, "This data item is not my responsibility", yield);
                }
            }

            MutableDataItem item {
                public_key,
                salt,
                value,
                sequence_number,
                { signature }
            };
            if (!item.verify()) {
                return send_error(206, "Invalid signature", yield);
            }

            boost::optional<MutableDataItem> existing_item = _data_store->get_mutable(target);
            if (existing_item) {
                if (sequence_number < existing_item->sequence_number) {
                    return send_error(302, "Sequence number less than current", yield);
                }

                if (
                       sequence_number == existing_item->sequence_number
                    && bencoding_encode(value) != bencoding_encode(existing_item->value)
                ) {
                    return send_error(302, "Sequence number not updated", yield);
                }

                boost::optional<int64_t> compare_and_swap_ = arguments["cas"].as_int();
                if (compare_and_swap_ && *compare_and_swap_ != existing_item->sequence_number) {
                    return send_error(301, "Compare-and-swap mismatch", yield);
                }
            }

            _data_store->put_mutable(item);

            return send_reply({}, yield);
        } else {
            /*
             * This is an immutable data item.
             */
            NodeID target = _data_store->immutable_get_id(value);

            if (!_data_store->verify_token(sender.address(), target, *token_)) {
                return send_error(203, "Incorrect put token", yield);
            }

            /*
             * Reject put requests for which there are more than enough
             * better responsible known nodes.
             *
             * TODO: This can be done in a more efficient way once the routing
             * table code stabilizes.
             */
            {
                bool contains_self = false;
                std::vector<NodeContact> closer_nodes = _routing_table->find_closest_routing_nodes(target, RESPONSIBLE_TRACKERS_PER_SWARM * 4);
                for (auto& i : closer_nodes) {
                    if (target.closer_to(_node_id, i.id)) {
                        contains_self = true;
                    }
                }
                if (!contains_self) {
                    return send_error(201, "This data item is not my responsibility", yield);
                }
            }

            _data_store->put_immutable(value);

            return send_reply({}, yield);
        }
    } else {
        return send_error(204, "Query type not implemented", yield);
    }
}

std::expected<asio::ip::udp::endpoint, sys::error_code>
resolve(
    asio::ip::udp ipv,
    const std::string& addr,
    const std::string& port,
    const std::shared_ptr<dns::Resolver>& dns_resolver,
    Async yield
) {
    using UdpLookup = udp::resolver::results_type;
    using UdpEndpoint = typename UdpLookup::endpoint_type;
    using Answers = std::vector<asio::ip::address>;
    UdpLookup results;

    auto answers = dns_resolver->resolve(addr, yield);

    if (!answers) {
        return std::unexpected(answers.error());
    }

    string_view port_strv = port;
    auto port_int = parse::number<uint16_t>(port_strv).value();
    util::AddrsAsEndpoints<Answers, UdpEndpoint> eps{std::move(*answers), port_int};
    results = UdpLookup::create(eps.begin(), eps.end(), addr, port);

    for (const auto& result : results) {
        auto ep = result.endpoint();

        if (ep.address().is_v4() && ipv == udp::v4()) {
            return ep;
        } else if (ep.address().is_v6() && ipv == udp::v6()) {
            return ep;
        }
    }

    return std::unexpected(sys::error_code(asio::error::not_found));
}

std::expected<DhtNode::BootstrapResult, sys::error_code>
DhtNode::bootstrap_single( bootstrap::Address bootstrap_address
                         , Async yield)
{
    std::expected<udp::endpoint, sys::error_code> bootstrap_ep =
        util::apply(
            bootstrap_address,
            [&] (const udp::endpoint& ep) {
                return ep;
            },
            [&] (const asio::ip::address& addr) {
                return udp::endpoint{addr, bootstrap::default_port};
            },
            [&] (const std::string& addr) {
                string_view hp(addr), host, port;
                std::tie(host, port) = util::split_ep(hp);
                auto ep = resolve(
                        _multiplexer->is_v4() ? udp::v4() : udp::v6(),
                        std::string(host),
                        port.empty() ? util::str(bootstrap::default_port) : std::string(port),
                        _dns_resolver,
                        yield
                    );

                if (!ep) {
                    LOG_DEBUG(yield, "Unable to resolve bootstrap server, giving up: "
                                   , addr, "; error=", ep.error());
                }

                return *ep;
            }
        );

    if (!bootstrap_ep) {
        return std::unexpected(bootstrap_ep.error());
    }

    auto initial_ping_reply = send_query_await_reply(
        { *bootstrap_ep, boost::none },
        "ping",
        BencodedMap{{ "id" , _node_id.to_bytestring() }},
        nullptr,
        nullptr,
        yield
    );

    if (!initial_ping_reply) {
        LOG_DEBUG(yield, " Bootstrap server does not reply, giving up: "
                       , bootstrap_address, "; error=", initial_ping_reply.error());
        return std::unexpected(initial_ping_reply.error());
    }

    auto my_ip = (*initial_ping_reply)["ip"].as_string_view();

    if (!my_ip) {
        LOG_DEBUG(yield, " Unexpected bootstrap server reply, giving up (no IP)");
        LOG_DEBUG(yield, "   ", *initial_ping_reply);
        return std::unexpected(asio::error::fault);
    }

    std::optional<asio::ip::udp::endpoint> my_endpoint = decode_endpoint(*my_ip);

    if (!my_endpoint) {
        LOG_DEBUG(yield, " Unexpected bootstrap server reply, giving up (can't parse IP)");
        LOG_DEBUG(yield, "   ", *initial_ping_reply);
        return std::unexpected(asio::error::fault);
    }

    return BootstrapResult{ *my_endpoint, *bootstrap_ep };
}

std::expected<void, sys::error_code> DhtNode::bootstrap(Async yield)
{
    auto cancel_con = _cancel.connect([&] { yield.cancel(); });

    auto metrics = _metrics.bootstrap();

    auto bootstraps = _bootstrap_config.collect();
    auto old_contacts = read_stored_contacts(stored_contacts_path(), yield)
        .value_or(std::set<NodeContact>{});

    for (auto& c : old_contacts) {
        bootstraps.push_back(c.endpoint);
    }

    udp::endpoint my_endpoint;
    std::set<udp::endpoint> node_endpoints;

    {
        constexpr size_t SCORE_GOAL = 5;

        using MyEndpoint   = udp::endpoint;
        using NodeEndpoint = udp::endpoint;

        std::random_device random_device;
        auto rng = std::default_random_engine(random_device());
        std::shuffle(bootstraps.begin(), bootstraps.end(), rng);

        struct Stats {
            size_t score;
            std::set<NodeEndpoint> nodes;
        };

        auto add_result = [] (auto& results, auto result, size_t score) -> Stats& {
            auto p = results.insert({result.my_ep, {score, {result.node_ep}}});
            auto& stats = p.first->second;
            if (p.second) return stats;
            if (stats.nodes.insert(result.node_ep).second) {
                stats.score += score;
            }
            return stats;
        };

        auto score_of = [](const bootstrap::Address a) {
            // We don't necessarily fully trust the nodes we know from previous
            // app runs. Thus we require SCORE_GOAL of them to respond with the
            // same (our) IP address to consider them trust-worthy.
            return util::apply(a, [](const udp::endpoint&)     { return size_t(1); }
                                // These come from the user or this code, trust them.
                                , [](const asio::ip::address&) { return SCORE_GOAL; }
                                , [](const std::string&)       { return SCORE_GOAL; });
        };

        while (true) {
            using namespace std::chrono;

            Async child_yield(yield);

            std::map<MyEndpoint, Stats> results;
            WaitCondition wc(_exec);
            size_t k = 0;

            try {
                for (const auto &bs : bootstraps) {
                    child_yield.spawn([
                        &,
                        lock = wc.lock(),
                        bs = bs
                    ] (Async yield) {
                        LOG_DEBUG(yield, " Bootstrapping node: ", bs, "...");

                        auto result = bootstrap_single(bs, yield);
                        LOG_DEBUG(yield, " Bootstrapping node: ", bs, ": done; result=", debug(result));

                        if (!result || !_peer_filter.is_allowed(result->my_ep)) return;

                        auto& stats = add_result(results, *result, score_of(bs));

                        if (stats.score >= SCORE_GOAL) {
                            my_endpoint = result->my_ep;
                            node_endpoints = std::move(stats.nodes);
                            child_yield.cancel();
                        }
                    });

                    // Try enough nodes quickly in parallel. Then try the rest with
                    // 300ms delays.
                    k += score_of(bs);
                    if (k < SCORE_GOAL) {
                        async_sleep(milliseconds(300), child_yield);
                    }
                }
            } catch (Async::Cancelled& e) {
                if (yield.is_cancelled()) {
                    throw e;
                }
            }

            wc.wait(yield);

            if (node_endpoints.size()) break;

            // We did not get enough score, but perhaps we have at least
            // something. If so, let's use that.
            if (results.size()) {
                size_t max_score = 0;
                for (auto r : results) {
                    if (r.second.score > max_score) {
                        my_endpoint = r.first;
                        node_endpoints = std::move(r.second.nodes);
                        max_score = r.second.score;
                    }
                }
                if (max_score) break;
            }

            // We could not bootstrap off any of the known nodes, wait a bit
            // and try again.
            async_sleep(seconds(10), yield);
        }
    }

    assert(node_endpoints.size());

    _wan_endpoint = my_endpoint;

    LOG_INFO(yield, " WAN endpoint: ", _wan_endpoint);

    auto send_ping_fn = [&] (const NodeContact& c) { send_ping(c); };
    _node_id = NodeID::generate(_wan_endpoint.address());
    _routing_table = std::make_unique<RoutingTable>(_node_id, send_ping_fn);

    for (auto c : old_contacts) {
        _routing_table->try_add_node(c, false);
    }

    /*
     * TODO: Make bootstrap node handling and ID determination more reliable.
     *
     * Ideally, this process should start a coroutine that continuously tries
     * to keep a list of hardcoded bootstrap servers up to date, resolving and
     * pinging them; and the find_node procedure can then use these endpoints
     * as additional start points for any node searches.
     *
     * There also needs to be vastly more retrying and fallbacks here.
     */

    for (auto ep : node_endpoints) {
        _bootstrap_endpoints.push_back(ep);
    }

    /*
     * Lookup our own ID, constructing a basic path to ourselves.
     */
    auto contacts = compat([&](Cancel cancel, asio::yield_context yield) {
        return find_closest_nodes(_node_id, cancel, yield);
    })(yield);
    if (!contacts) {
        return std::unexpected(contacts.error());
    }

    /*
     * We now know enough nodes that general DHT queries should succeed. The
     * remaining work is part of our participation in the DHT, but is not
     * necessary for implementing queries.
     */
    _ready = true;
    metrics.mark_success();

    return {};
}


template<class Evaluate>
requires std::invocable<
    Evaluate,
    const Contact&,
    WatchDog&,
    util::AsyncQueue<NodeContact>&,
    Async
>
std::expected<void, sys::error_code>
DhtNode::collect(
    DebugCtx& dbg,
    const NodeID& target_id,
    Evaluate&& evaluate,
    Async yield
) {
    auto canceled = _cancel.connect([&] { yield.cancel(); });

    if (!_routing_table) {
        // We're not yet bootstrapped.
        return std::unexpected(asio::error::try_again);
    }

    // (Note: can't use lambda because we need default constructibility now)
    struct Compare {
        NodeID target_id;

        // Bootstrap nodes (those with id == boost::none) shall be ordered
        // last.
        bool operator()(const Contact& l, const Contact& r) const {
            if (!l.id && !r.id) return l.endpoint < r.endpoint;
            if ( l.id && !r.id) return true;
            if (!l.id &&  r.id) return false;
            return target_id.closer_to(*l.id, *r.id);
        }
    };

    using CandidateSet = std::set<Contact, Compare>;

    CandidateSet seed_candidates(Compare{target_id});

    std::set<udp::endpoint> added_endpoints;

    auto table_contacts =
        _routing_table->find_closest_routing_nodes(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);

    for (auto& contact : table_contacts) {
        seed_candidates.insert(contact);
        added_endpoints.insert(contact.endpoint);
    }

    for (auto ep : _bootstrap_endpoints) {
        if (added_endpoints.count(ep) != 0) continue;
        seed_candidates.insert({ ep, boost::none });
    }

    ::ouinet::bittorrent::collect(
        dbg,
        std::move(seed_candidates),
        std::forward<Evaluate>(evaluate),
        yield
    );

    return {};
}

std::vector<NodeContact> DhtNode::find_closest_nodes(
    NodeID target_id,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    sys::error_code ec;
    ProximityMap<udp::endpoint> out(target_id, RESPONSIBLE_TRACKERS_PER_SWARM);

    DebugCtx dbg;
    dbg.enable_log = SPEED_DEBUG;

    compat([&](Async yield) {
        return collect(dbg, target_id, [&](
            const Contact& candidate,
            WatchDog& dms,
            util::AsyncQueue<NodeContact>& closer_nodes,
            Async yield
        ) {
            if (!candidate.id && out.full()) {
                return;
            }

            if (candidate.id && !out.would_insert(*candidate.id)) {
                return;
            }

            auto accepted = query_find_node2( target_id
                                            , candidate
                                            , closer_nodes
                                            , dms
                                            , &dbg
                                            , yield);

            if (accepted && *accepted && candidate.id) {
                out.insert({ *candidate.id, candidate.endpoint });
            }

            return;
        }, yield);
    })(cancel_signal, yield[ec]);

    std::vector<NodeContact> output_set;
    for (auto& c : out) {
        output_set.push_back({ c.first, c.second });
    }

    return or_throw<std::vector<NodeContact>>(yield, ec, std::move(output_set));
}

std::expected<BencodedMap, sys::error_code> DhtNode::send_ping(
    NodeContact contact,
    Async yield
) {
    return send_query_await_reply(
        contact,
        "ping",
        BencodedMap{{ "id", _node_id.to_bytestring() }},
        nullptr,
        nullptr,
        yield
    );
}

void DhtNode::send_ping(NodeContact contact)
{
    // It is currently expected that this function returns immediately, due to
    // that we need to spawn an unlimited number of coroutines.  Perhaps it
    // would be better if functions using this send_ping function would only
    // spawn a limited number of coroutines and use only that.
    task::spawn_detached(_exec, [
        this,
        contact,
        cancel = _cancel,
        log_path = _log_path
    ] (asio::yield_context yield) mutable {
        std::ignore = send_ping(contact, Async(yield, cancel, std::move(log_path)));
    });
}

/*
 * Send a query that writes data to the DHT. Repeat up to 5 times until we
 * get a positive response.
 */
std::expected<void, sys::error_code> DhtNode::send_write_query(
    udp::endpoint destination,
    NodeID destination_id,
    const std::string& query_type,
    const BencodedMap& query_arguments,
    Async yield
) {
    /*
     * Retry the write message a couple of times.
     */
    const int TRIES = 3;

    sys::error_code last_error;

    for (int i = 0; i < TRIES; i++) {
        auto result = send_query_await_reply(
            { destination, destination_id },
            query_type,
            query_arguments,
            nullptr,
            nullptr,
            yield
        );

        if (result) {
            return {};
        } else {
            last_error = result.error();
        }
    }

    return std::unexpected(last_error);
}

/**
 * Send a find_node query to a target node, and parse the reply.
 * @return True when received a valid response, false otherwise.
 */
// http://bittorrent.org/beps/bep_0005.html#find-node
bool DhtNode::query_find_node(
    NodeID target_id,
    Contact node,
    std::vector<NodeContact>& closer_nodes,
    Async yield
) {
    auto find_node_reply = send_query_await_reply(
        node,
        "find_node",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "target", target_id.to_bytestring() }
        },
        nullptr,
        nullptr,
        yield
    );
    if (!find_node_reply) {
        return false;
    }
    if ((*find_node_reply)["y"] != "r") {
        return false;
    }
    BencodedMap* response = (*find_node_reply)["r"].as_map();
    if (!response) {
        return false;
    }

    if (is_v4()) {
        auto nodes = (*response)["nodes"].as_string_view();
        if (!NodeContact::decode_compact_v4(*nodes, closer_nodes)) {
            return false;
        }
    } else {
        auto nodes6 = (*response)["nodes6"].as_string_view();
        if (!NodeContact::decode_compact_v6(*nodes6, closer_nodes)) {
            return false;
        }
    }

    return !closer_nodes.empty();
}

std::expected<bool, sys::error_code> DhtNode::query_find_node2(
    NodeID target_id,
    Contact node,
    util::AsyncQueue<NodeContact>& closer_nodes,
    WatchDog& dms,
    DebugCtx* dbg,
    Async yield
) {
    auto find_node_reply = send_query_await_reply(
        node,
        "find_node",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "target", target_id.to_bytestring() }
        },
        &dms,
        dbg,
        yield
    );
    if (!find_node_reply) {
        return std::unexpected(find_node_reply.error());
    }

    if ((*find_node_reply)["y"] != "r") {
        return false;
    }

    BencodedMap* response = (*find_node_reply)["r"].as_map();
    if (!response) {
        return false;
    }

    return read_nodes(is_v4(), *response, _peer_filter, closer_nodes, yield);
}

// http://bittorrent.org/beps/bep_0005.html#get-peers
std::optional<BencodedMap> DhtNode::query_get_peers(
    NodeID infohash,
    Contact node,
    util::AsyncQueue<NodeContact>& closer_nodes,
    WatchDog& dms,
    DebugCtx* dbg,
    Async yield
) {
    auto get_peers_reply = send_query_await_reply(
        node,
        "get_peers",
        BencodedMap {
            { "id", _node_id.to_bytestring() },
            { "info_hash", infohash.to_bytestring() }
        },
        &dms,
        dbg,
        yield
    );
    if (!get_peers_reply) {
        return std::nullopt;
    }
    if ((*get_peers_reply)["y"] != "r") {
        return std::nullopt;
    }
    BencodedMap* response = (*get_peers_reply)["r"].as_map();
    if (!response) {
        return std::nullopt;
    }

    std::vector<NodeContact> closer_nodes_v;

    if (is_v4()) {
        auto nodes = (*response)["nodes"].as_string_view();
        if (!NodeContact::decode_compact_v4(*nodes, closer_nodes_v)) {
            return std::nullopt;
        }
    } else {
        auto nodes6 = (*response)["nodes6"].as_string_view();
        if (!NodeContact::decode_compact_v6(*nodes6, closer_nodes_v)) {
            return std::nullopt;
        }
    }

    if (closer_nodes_v.empty()) {
        /*
         * We got a reply to get_peers, but it does not contain nodes.
         * Follow up with a find_node to fill the gap.
         */
        std::ignore = query_find_node(infohash, node, closer_nodes_v, yield);
    }

    std::ignore = compat([&](Cancel cancel, asio::yield_context yield) {
        closer_nodes.async_push_many(closer_nodes_v, cancel, yield);
    })(yield);

    return std::move(*response);
}

// http://bittorrent.org/beps/bep_0044.html#get-message
boost::optional<BencodedMap> DhtNode::query_get_data(
    NodeID key,
    Contact node,
    util::AsyncQueue<NodeContact>& closer_nodes,
    WatchDog& dms,
    DebugCtx* dbg,
    Cancel& cancel,
    asio::yield_context yield
) {
    sys::error_code ec;

    BencodedMap get_reply = compat([&](Async yield) {
        return send_query_await_reply(
            node,
            "get",
            BencodedMap {
                { "id", _node_id.to_bytestring() },
                { "target", key.to_bytestring() }
            },
            nullptr,
            nullptr,
            yield
        );
    })(cancel, yield[ec]);

    if (ec == asio::error::operation_aborted) {
        return boost::none;
    }

    if (ec) {
        /*
         * Ideally, nodes that do not implement BEP 44 would reply to this
         * query with a "not implemented" error. But in practice, most do not
         * reply at all. If such nodes make up the entire routing table (as is
         * often the case), the lookup might fail entirely. But doing an entire
         * search through nodes without BEP 44 support slows things down quite
         * a lot. Hm.
         *
         * TODO: Perhaps using a separate routing table for BEP 44 nodes would
         * improve things here?
         */
        compat([&](Async yield) {
            return query_find_node2(
                key,
                node,
                closer_nodes,
                dms, dbg,
                yield
            );
        })(cancel, yield);

        return boost::none;
    }

    if (get_reply["y"] != "r") {
        /*
         * This is probably a node that does not implement BEP 44.
         * Query it using find_node instead. Ignore errors and hope for
         * the best; we are just trying to find some closer nodes here.
         */
        compat([&](Async yield) {
            return query_find_node2(
                key,
                node,
                closer_nodes,
                dms, dbg,
                yield
            );
        })(cancel, yield);

        return boost::none;
    }

    BencodedMap* response = get_reply["r"].as_map();

    if (!response) return boost::none;

    compat([&](Async yield) {
        return read_nodes(is_v4(), *response, _peer_filter, closer_nodes, yield);
    })(cancel, yield[ec]);

    return {std::move(*response)};
}

boost::optional<BencodedMap> DhtNode::query_get_data2(
    NodeID key,
    Contact node,
    util::AsyncQueue<NodeContact>& closer_nodes,
    WatchDog& dms,
    DebugCtx& dbg,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    sys::error_code ec;

    assert(!cancel_signal);
    dms.expires_after( _stats->max_reply_wait_time("get")
                     + _stats->max_reply_wait_time("find_node"));

    Cancel local_cancel(cancel_signal);
    WaitCondition wc(_exec);

    // Ideally, nodes that do not implement BEP 44 would reply to this query
    // with a "not implemented" error. But in practice, most do not reply at
    // all. If such nodes make up the entire routing table (as is often the
    // case), the lookup might fail entirely. But doing an entire search
    // through nodes without BEP 44 support slows things down quite a lot.
    WatchDog wd(_exec, _stats->max_reply_wait_time("get"), [&] () mutable {
        if (local_cancel) return;
        task::spawn_detached(_exec, [&, lock = wc.lock()] ( asio::yield_context yield) {
            if (dbg) cerr << dbg << "query_find_node2 start " << node << "\n";
            sys::error_code ec;
            compat([&](Async yield) {
                return query_find_node2(key, node, closer_nodes, dms, &dbg, yield);
            })(local_cancel, yield[ec]);
            if (dbg) cerr << dbg << "query_find_node2 end " << node << "\n";
            local_cancel();
        });
    });

    assert(!cancel_signal);
    assert(!local_cancel);
    if (dbg) cerr << dbg << "send_query_await_reply get start " << node << "\n";
    BencodedMap get_reply = compat([&](Async yield) {
        return send_query_await_reply(
            node,
            "get",
            BencodedMap {
                { "id", _node_id.to_bytestring() },
                { "target", key.to_bytestring() }
            },
            &dms,
            &dbg,
            yield
        );
    })(local_cancel, yield[ec]);

    if (dbg) cerr << dbg << "send_query_await_reply get end: " << node << "; ec=" << util::str(ec) << "\n";
    sys::error_code ec_;

    if (cancel_signal) ec = asio::error::operation_aborted;

    if (ec || get_reply["y"] != "r") {
        wc.wait(yield[ec_]);
        return boost::none;
    }

    local_cancel();
    wc.wait(yield[ec_]);

    std::vector<NodeContact> closer_nodes_v;

    BencodedMap* response = get_reply["r"].as_map();

    if (!response) return boost::none;

    compat([&](Async yield) {
        return read_nodes(is_v4(), *response, _peer_filter, closer_nodes, yield);
    })(cancel_signal, yield[ec]);

    return {std::move(*response)};
}

boost::optional<BencodedMap> DhtNode::query_get_data3(
    NodeID key,
    Contact node,
    util::AsyncQueue<NodeContact>& closer_nodes,
    WatchDog& dms,
    DebugCtx& dbg,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    sys::error_code ec;

    assert(!cancel_signal);
    //dms.expires_after( _stats->max_reply_wait_time("get")
    //                 + _stats->max_reply_wait_time("find_node"));

    Cancel local_cancel(cancel_signal);
    //WaitCondition wc(_exec);

    assert(!cancel_signal);
    assert(!local_cancel);
    if (dbg) cerr << dbg << "send_query_await_reply get start " << node << "\n";
    BencodedMap get_reply = compat([&](Async yield) {
        return send_query_await_reply(
            node,
            "get",
            BencodedMap {
                { "id", _node_id.to_bytestring() },
                { "target", key.to_bytestring() }
            },
            &dms,
            &dbg,
            yield
        );
    })(local_cancel, yield[ec]);

    if (dbg) cerr << dbg << "send_query_await_reply get end: " << node << "; ec=" << util::str(ec) << "\n";

    if (cancel_signal) ec = asio::error::operation_aborted;

    if (ec || get_reply["y"] != "r") {
        return boost::none;
    }

    local_cancel();

    std::vector<NodeContact> closer_nodes_v;

    BencodedMap* response = get_reply["r"].as_map();

    if (!response) return boost::none;

    compat([&](Async yield) {
        return read_nodes(is_v4(), *response, _peer_filter, closer_nodes, yield);
    })(cancel_signal, yield[ec]);

    return {std::move(*response)};
}

/**
 * Perform a get_peers search. Returns the peers found, as well as necessary
 * data to later perform an announce operation.
 */
std::expected<void, sys::error_code> DhtNode::tracker_do_search_peers(
    NodeID infohash,
    std::set<udp::endpoint>& peers,
    std::map<NodeID, TrackerNode>& responsible_nodes,
    Async yield
) {
    struct ResponsibleNode {
        asio::ip::udp::endpoint node_endpoint;
        std::vector<udp::endpoint> peers;
        std::string put_token;
    };
    ProximityMap<ResponsibleNode> responsible_nodes_full(infohash, RESPONSIBLE_TRACKERS_PER_SWARM);

    DebugCtx dbg;
    auto result = collect(dbg, infohash, [&](
        const Contact& candidate,
        WatchDog& wd,
        util::AsyncQueue<NodeContact>& closer_nodes,
        Async yield
    ) {
        if (!candidate.id && responsible_nodes_full.full()) {
            return;
        }
        if (candidate.id && !responsible_nodes_full.would_insert(*candidate.id)) {
            return;
        }

        auto response_ = query_get_peers(
            infohash,
            candidate,
            closer_nodes,
            wd,
            &dbg,
            yield
        );
        if (!response_) return;

        BencodedMap& response = *response_;

        boost::optional<std::string> announce_token = response["token"].as_string();

        if (!announce_token) return;

        if (candidate.id) {
            ResponsibleNode node{ candidate.endpoint, {}, std::move(*announce_token) };
            BencodedList* encoded_peers = response["values"].as_list();
            if (encoded_peers) {
                for (auto& peer : *encoded_peers) {
                    auto peer_string = peer.as_string_view();
                    if (!peer_string) continue;

                    std::optional<udp::endpoint> endpoint = decode_endpoint(*peer_string);
                    if (!endpoint) continue;

                    node.peers.push_back(*endpoint);
                }
            }
            responsible_nodes_full.insert({ *candidate.id, std::move(node) });
        }
    }, yield);

    peers.clear();
    responsible_nodes.clear();
    for (auto& i : responsible_nodes_full) {
        peers.insert(i.second.peers.begin(), i.second.peers.end());
        responsible_nodes[i.first] = { i.second.node_endpoint, i.second.put_token };
    }

    return result;
}

std::ostream& operator << (std::ostream& os, const DhtNode::BootstrapResult& result) {
    return os << "{ my_ep=" << result.my_ep << ", node_ep=" << result.node_ep << " }";
}

// -------------------------------------------------------------------------------------------------

MainlineDht::MainlineDht( const AsioExecutor& exec
                        , metrics::MainlineDht metrics
                        , std::shared_ptr<dns::Resolver> dns_resolver
                        , uint32_t mux_rx_limit
                        , fs::path storage_dir
                        , bootstrap::Config bootstrap_config
                        , util::LogPath log_path)
    : _exec(exec)
    , _ready_cv(exec)
    , _dns_resolver(std::move(dns_resolver))
    , _mux_rx_limit(mux_rx_limit)
    , _storage_dir(std::move(storage_dir))
    , _bootstrap_config(std::move(bootstrap_config))
    , _metrics(std::move(metrics))
    , _log_path(std::move(log_path))
{
}

MainlineDht::~MainlineDht()
{
    _cancel();
}

void MainlineDht::set_endpoints(const std::set<udp::endpoint>& eps)
{
    // Remove nodes whose address is not listed in `eps`
    for (auto it = _nodes.begin(); it != _nodes.end(); ) {
        if (eps.count(it->first)) {
            ++it;
        } else {
            it = _nodes.erase(it);
        }
    }

    // Ensure that there are nodes for each address in `eps` (create if needed)
    for (auto ep : eps) {
        if (_nodes.count(ep)) continue;

        asio_utp::udp_multiplexer m(_exec);
        sys::error_code ec;
        m.bind(ep, ec);
        assert(!ec);
        if (ec) continue;

        (void) add_endpoint(std::move(m));
    }
}

metrics::DhtNode metrics_dht_node_for(metrics::MainlineDht& metrics, const asio::ip::address& addr) {
    if (addr.is_v4()) {
        return metrics.dht_node_ipv4();
    } else {
        assert(addr.is_v6());
        return metrics.dht_node_ipv6();
    }
}

std::set<udp::endpoint> MainlineDht::wan_endpoints() const {
    std::set<udp::endpoint> ret;
    for (auto& p : _nodes) { ret.insert(p.second->wan_endpoint()); }
    return ret;
}

bool MainlineDht::all_ready() const {
    for (const auto& n : _nodes) {
        if (!n.second->ready()) return false;
    }
    return true;
}

void MainlineDht::stop() {
    _cancel();
    _nodes.clear();
}

Promise<asio::ip::udp::endpoint>::Future
MainlineDht::add_endpoint(asio_utp::udp_multiplexer m)
{
    auto local_ep = m.local_endpoint();

    auto& node = _nodes[local_ep] = make_unique<DhtNode>(
        _exec,
        metrics_dht_node_for(_metrics, local_ep.address()),
        _dns_resolver,
        _mux_rx_limit,
        _storage_dir,
        _bootstrap_config,
        _log_path
    );
    node->set_peer_filter(_peer_filter);

    Promise<asio::ip::udp::endpoint> promise(_exec);
    auto future = promise.get_future();

    task::spawn_detached(
        _exec,
        [
            this,
            log_path = _log_path,
            m = std::move(m),
            promise = std::move(promise),
            local_ep
        ](auto y) mutable {
            Async yield(y, _cancel, std::move(log_path));

            auto cancelled = yield.cancel_slot([&] {
                if (auto it = _nodes.find(local_ep); it != _nodes.end()) {
                    _nodes.erase(it);
                }
            });

            auto& node = _nodes[local_ep];
            auto result = node->start(std::move(m), yield);

            if (result) {
                promise.set_value(node->wan_endpoint());
            }

            // TODO: should we send errors as well?

            _ready_cv.notify();
        }
    );

    return future;
}

std::expected<std::set<udp::endpoint>, sys::error_code>
MainlineDht::tracker_announce(
    NodeID infohash,
    std::optional<int> port,
    Async yield
) {
    auto cc = _cancel.connect([&] { yield.cancel(); });

    std::expected<std::set<udp::endpoint>, sys::error_code> output =
        std::unexpected(asio::error::network_unreachable);

    WaitCondition wc(yield.get_executor());

    for (auto& i : _nodes) {
        yield.spawn([
            &,
            node = i.second.get(),
            lock = wc.lock()
        ] (Async yield) {
            auto peers = node->tracker_announce(infohash, port, yield);

            if (peers) {
                if (output) {
                    output->insert(peers->begin(), peers->end());
                } else {
                    output = std::move(peers);
                }
            } else {
                if (!output) {
                    output = std::unexpected(peers.error());
                }
            }
        });
    }

    wc.wait(yield);

    return output;
}

std::expected<std::set<udp::endpoint>, sys::error_code>
MainlineDht::tracker_get_peers(NodeID infohash, Async yield)
{
    auto terminated = _cancel.connect([&] { yield.cancel(); });

    std::expected<std::set<udp::endpoint>, sys::error_code> output =
        std::unexpected(asio::error::network_unreachable);

    WaitCondition wc(yield.get_executor());

    for (auto& i : _nodes) {
        yield.spawn([&, lock = wc.lock()] (auto yield) {
            if (!i.second->ready()) {
                return;
            }

            auto peers = i.second->tracker_get_peers(infohash, yield);

            if (peers) {
                if (output) {
                    output->insert(peers->begin(), peers->end());
                } else {
                    output = std::move(*peers);
                }
            } else {
                if (!output) {
                    output = std::unexpected(peers.error());
                }
            }
        });
    }

    wc.wait(yield);

    return output;
}

boost::optional<BencodedValue> MainlineDht::immutable_get(
        NodeID key,
        Cancel& cancel_signal,
        asio::yield_context yield
) {
    std::optional<BencodedValue> output;
    sys::error_code ec;

    Cancel cancel_attempts;

    SuccessCondition success_condition(_exec);
    WaitCondition completed_condition(_exec);
    for (auto& i : _nodes) {
        task::spawn_detached(_exec, [
            &,
            success = success_condition.lock(),
            complete = completed_condition.lock()
        ] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            auto data = compat(
                [&](Async yield) -> std::expected<std::optional<BencodedValue>, sys::error_code> {
                    return i.second->data_get_immutable(key, yield);
                }
            )(cancel_attempts, yield[ec]);

            if (!ec && data) {
                output = data;
                success.release(true);
            }
        });
    }
    auto cancelled = cancel_signal.connect([&] {
        success_condition.cancel();
    });

    auto terminated = _cancel.connect([&] {
        success_condition.cancel();
    });

    if (!success_condition.wait_for_success(yield)) {
        if (success_condition.cancelled()) {
            ec = asio::error::operation_aborted;
        } else {
            ec = asio::error::not_found;
        }
    }

    cancel_attempts();

    completed_condition.wait(yield);

    return or_throw<boost::optional<BencodedValue>>(yield, ec);
}

void MainlineDht::mutable_put(
    const MutableDataItem& data,
    Cancel& top_cancel,
    asio::yield_context yield
) {
    Cancel cancel(top_cancel);

    SuccessCondition condition(_exec);
    WaitCondition wait_all(_exec);

    for (auto& i : _nodes) {
        task::spawn_detached(_exec, [
            &,
            lock = condition.lock(),
            lock_all = wait_all.lock()
        ] (asio::yield_context yield) {
            if (!i.second->ready()) {
                return;
            }

            sys::error_code ec;
            i.second->data_put_mutable(data, cancel, yield[ec]);

            if (ec) return;

            lock.release(true);
        });
    }

    auto cancelled = cancel.connect([&] {
        condition.cancel();
    });

    auto terminated = _cancel.connect([&] {
        condition.cancel();
    });

    sys::error_code ec;

    if (condition.wait_for_success(yield)) {
        cancel();
    } else {
        if (condition.cancelled()) { ec = asio::error::operation_aborted;   }
        else                       { ec = asio::error::network_unreachable; }
    }

    wait_all.wait(yield);

    return or_throw(yield, ec);
}

boost::optional<MutableDataItem> MainlineDht::mutable_get(
    const sign::PublicKey& public_key,
    boost::string_view salt,
    Cancel& cancel_signal,
    asio::yield_context yield
) {
    boost::optional<MutableDataItem> output;
    sys::error_code ec;

    Cancel cancel_attempts;

    SuccessCondition success_condition(_exec);
    WaitCondition completed_condition(_exec);

    for (auto& i : _nodes) {
        task::spawn_detached(_exec, [
            &,
            success = success_condition.lock(),
            complete = completed_condition.lock()
        ] (asio::yield_context yield) {
            //if (!i.second->ready()) {
            //    return;
            //}

            sys::error_code ec;
            boost::optional<MutableDataItem> data = i.second->data_get_mutable(
                public_key,
                salt,
                cancel_attempts,
                yield[ec]
            );

            if (!ec && data) {
                output = data;
                success.release(true);
            }
        });
    }
    auto cancelled = cancel_signal.connect([&] {
        success_condition.cancel();
    });
    auto terminated = _cancel.connect([&] {
        success_condition.cancel();
    });

    if (!success_condition.wait_for_success(yield)) {
        if (success_condition.cancelled()) {
            ec = asio::error::operation_aborted;
        } else {
            ec = asio::error::not_found;
        }
    }

    cancel_attempts();

    completed_condition.wait(yield);

    return or_throw(yield, ec, std::move(output));
}

bool MainlineDht::is_peer_allowed(UdpEndpoint const& ep) const {
    return _peer_filter.is_allowed(ep);
}

void MainlineDht::set_peer_filter(PeerFilter filter) {
    _peer_filter = filter;
    for (auto& p : _nodes) {
        p.second->set_peer_filter(filter);
    }
}

void MainlineDht::wait_all_ready(Async yield) {
    auto cancelled = _cancel.connect([&] { yield.cancel(); });

    while (!all_ready()) {
        _ready_cv.wait(yield);
    }
}

std::ostream& operator<<(std::ostream& os, const Contact& c)
{
    os << "(Contact " << c.endpoint << " id:";
    if (c.id) {
        os << *c.id;
    } else {
        os << "none";
    }
    return os << ")";
}


} // bittorrent namespace
} // ouinet namespace
