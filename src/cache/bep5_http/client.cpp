#include "client.h"
#include "announcer.h"
#include "dht_lookup.h"
#include "local_peer_discovery.h"
#include "../dht_groups.h"
#include "../http_sign.h"
#include "../http_store.h"
#include "../../http_util.h"
#include "../../parse/number.h"
#include "../../util/wait_condition.h"
#include "../../util/set_io.h"
#include "../../util/async_generator.h"
#include "../../util/lru_cache.h"
#include "../../util/handler_tracker.h"
#include "../../bittorrent/dht.h"
#include "../../bittorrent/is_martian.h"
#include "../../ouiservice/utp.h"
#include "../../logger.h"
#include "../../async_sleep.h"
#include "../../constants.h"
#include "../../session.h"
#include "../../bep5_swarms.h"
#include <ctime>
#include <map>

using namespace std;
using namespace ouinet;
using namespace cache::bep5_http;
using udp = asio::ip::udp;

namespace fs = boost::filesystem;
namespace bt = bittorrent;

struct GarbageCollector {
    cache::AbstractHttpStore& http_store;  // for looping over entries
    cache::AbstractHttpStore::keep_func keep;  // caller-provided checks

    asio::executor _executor;
    Cancel _cancel;

    GarbageCollector( cache::AbstractHttpStore& http_store
                    , cache::AbstractHttpStore::keep_func keep
                    , asio::executor ex)
        : http_store(http_store)
        , keep(move(keep))
        , _executor(ex)
    {}

    ~GarbageCollector() { _cancel(); }

    void start()
    {
        asio::spawn(_executor, [&] (asio::yield_context yield) {
            TRACK_HANDLER();
            Cancel cancel(_cancel);

            LOG_DEBUG("Bep5HTTP: Garbage collector started");
            while (!cancel) {
                sys::error_code ec;
                async_sleep(_executor, chrono::minutes(7), cancel, yield[ec]);
                if (cancel || ec) break;

                LOG_DEBUG("Bep5HTTP: Collecting garbage...");
                http_store.for_each([&] (auto rr, auto y) {
                    sys::error_code e;
                    auto k = keep(std::move(rr), y[e]);
                    if (cancel) ec = asio::error::operation_aborted;
                    return or_throw(y, e, k);
                }, cancel, yield[ec]);
                if (ec) LOG_WARN("Bep5HTTP: Collecting garbage: failed"
                                 " ec:", ec.message());
                LOG_DEBUG("Bep5HTTP: Collecting garbage: done");
            }
            LOG_DEBUG("Bep5HTTP: Garbage collector stopped");
        });
    }
};

struct Client::Impl {
    // The newest protocol version number seen in a trusted exchange
    // (i.e. from injector-signed cached content).
    unsigned newest_proto_seen = http_::protocol_version_current;

    asio::executor ex;
    shared_ptr<bt::MainlineDht> dht;
    string uri_swarm_prefix;
    util::Ed25519PublicKey cache_pk;
    fs::path cache_dir;
    unique_ptr<cache::AbstractHttpStore> http_store;
    boost::posix_time::time_duration max_cached_age;
    Cancel lifetime_cancel;
    Announcer announcer;
    GarbageCollector gc;
    map<string, udp::endpoint> peer_cache;
    util::LruCache<bt::NodeID, unique_ptr<DhtLookup>> dht_lookups;
    log_level_t log_level = INFO;
    LocalPeerDiscovery local_peer_discovery;
    uint32_t debug_next_load_nr = 0;
    std::unique_ptr<DhtGroups> _dht_groups;


    bool log_debug() const { return log_level <= DEBUG; }
    bool log_info()  const { return log_level <= INFO; }

    Impl( shared_ptr<bt::MainlineDht> dht_
        , util::Ed25519PublicKey& cache_pk
        , fs::path cache_dir
        , unique_ptr<cache::AbstractHttpStore> http_store_
        , boost::posix_time::time_duration max_cached_age
        , log_level_t log_level)
        : ex(dht_->get_executor())
        , dht(move(dht_))
        , uri_swarm_prefix(bep5::compute_uri_swarm_prefix
              (cache_pk, http_::protocol_version_current))
        , cache_pk(cache_pk)
        , cache_dir(move(cache_dir))
        , http_store(move(http_store_))
        , max_cached_age(max_cached_age)
        , announcer(dht, log_level)
        , gc(*http_store, [&] (auto rr, auto y) {
              return keep_cache_entry(move(rr), y);
          }, ex)
        , dht_lookups(256)
        , log_level(log_level)
        , local_peer_discovery(ex, dht->local_endpoints())
    {}

    std::string compute_swarm_name(boost::string_view dht_group) const {
        return bep5::compute_uri_swarm_name(
                uri_swarm_prefix,
                dht_group);
    }

    void serve_local( const http::request<http::empty_body>& req
                    , GenericStream& sink
                    , Cancel& cancel
                    , asio::yield_context yield)
    {
        sys::error_code ec;

        // Usually we would
        // (1) check that the request matches our protocol version, and
        // (2) check that we can derive a key to look up the local cache.
        // However, we still want to blindly send a response we have cached
        // if the request looks like a Ouinet one and we can derive a key,
        // to help the requesting client get the result and other information
        // like a potential new protocol version.
        // The requesting client may choose to drop the response
        // or attempt to extract useful information from it.

        auto req_proto = req[http_::protocol_version_hdr];
        if (!boost::regex_match( req_proto.begin(), req_proto.end()
                               , http_::protocol_version_rx)) {
            if (log_debug()) {
                cerr << "Bep5HTTP: Not a Ouinet request\n";
            }
            return handle_bad_request(sink, req, yield[ec]);
        }

        auto key = key_from_http_req(req);
        if (!key) {
            if (log_debug()) {
                cerr << "Bep5HTTP: Cannot derive key from request\n";
            }
            return handle_bad_request(sink, req, yield[ec]);
        }

        auto rr = http_store->reader(*key, ec);
        if (ec) {
            if (!cancel && log_debug()) {
                cerr << "Bep5HTTP: Not Serving " << *key
                     << " ec:" << ec.message() << "\n";
            }
            return handle_not_found(sink, req, yield[ec]);
        }

        if (log_debug()) {
            cerr << "Bep5HTTP: Serving " << *key << "\n";
        }

        auto s = Session::create(move(rr), cancel, yield[ec]);
        if (!ec) s.flush_response(sink, cancel, yield[ec]);

        return or_throw(yield, ec);
    }

    std::size_t local_size( Cancel cancel
                          , asio::yield_context yield) const
    {
        return http_store->size(cancel, yield);
    }

    void local_purge( Cancel cancel
                    , asio::yield_context yield)
    {
        // TODO: avoid overlapping with garbage collector
        LOG_DEBUG("Bep5HTTP: Purging local cache...");

        sys::error_code ec;
        http_store->for_each([&] (auto, auto) {
            return false;  // remove all entries
        }, cancel, yield[ec]);
        if (ec) {
            LOG_ERROR("Bep5HTTP: Purging local cache: failed"
                      " ec:", ec.message());
            return or_throw(yield, ec);
        }

        LOG_DEBUG("Bep5HTTP: Purging local cache: done");
    }

    void handle_http_error( GenericStream& con
                          , const http::request<http::empty_body>& req
                          , http::status status
                          , const string& proto_error
                          , asio::yield_context yield)
    {
        auto res = util::http_client_error(req, status, proto_error);
        http::async_write(con, res, yield);
    }

    void handle_bad_request( GenericStream& con
                           , const http::request<http::empty_body>& req
                           , asio::yield_context yield)
    {
        return handle_http_error(con, req, http::status::bad_request, "", yield);
    }

    void handle_not_found( GenericStream& con
                         , const http::request<http::empty_body>& req
                         , asio::yield_context yield)
    {
        return handle_http_error( con, req, http::status::not_found
                                , http_::response_error_hdr_retrieval_failed, yield);
    }

    std::set<udp::endpoint> dht_get_peers( bt::NodeID infohash
                                         , Cancel& cancel
                                         , Yield yield)
    {
        auto* lookup = dht_lookups.get(infohash);

        if (!lookup) {
            lookup = dht_lookups.put( infohash
                                    , make_unique<DhtLookup>(dht, infohash));
        }

        return (*lookup)->get(cancel, yield);
    }

    Session load( const std::string& key
                , const std::string& dht_group
                , Cancel cancel
                , Yield yield)
    {
        boost::optional<decltype(debug_next_load_nr)> dbg;

        if (log_debug()) {
            dbg = debug_next_load_nr++;
        }

        namespace err = asio::error;

        {
            sys::error_code ec;
            auto rs = load_from_local(key, cancel, yield[ec]);
            if (dbg) yield.log(*dbg, " Bep5Http: looking up local cache ec:", ec.message());
            if (ec == err::operation_aborted) return or_throw<Session>(yield, ec);
            // TODO: Check its age, store it if it's too old but keep trying
            // other peers.
            if (!ec) return rs;
            // Try distributed cache on other errors.
        }

        auto canceled = lifetime_cancel.connect([&] { cancel(); });

        boost::optional<udp::endpoint> tried;

        enum Try { local_peers, last_known, dht_peers };

        static const vector<Try> _to_try{ local_peers, last_known, dht_peers };

        for (auto do_try : _to_try) {
            if (cancel) break;
            sys::error_code ec;

            set<udp::endpoint> eps;

            if (do_try == local_peers) {
                eps = local_peer_discovery.found_peers();
                if (eps.empty()) continue;
            }
            else if (do_try == last_known) {
                auto peer_i = peer_cache.find(dht_group);
                if (peer_i == peer_cache.end()) continue;
                auto ep = peer_i->second;
                if (dbg) {
                    yield.log(*dbg, " Bep5Http: using cached endpoint first:", ep);
                }
                eps = {ep};
                tried = ep;
            }
            else if (do_try == dht_peers) {
                auto swarm_name = compute_swarm_name(dht_group);
                bt::NodeID infohash = util::sha1_digest(swarm_name);

                if (dbg) {
                    yield.log(*dbg, " Bep5Http: DHT lookup:");
                    yield.log(*dbg, "     key:        ", key);
                    yield.log(*dbg, "     dht_group:  ", dht_group);
                    yield.log(*dbg, "     swarm_name: ", swarm_name);
                    yield.log(*dbg, "     infohash:   ", infohash);
                }

                eps = dht_get_peers(infohash, cancel, yield[ec]);

                if (cancel) return or_throw<Session>(yield, err::operation_aborted);
                // TODO: Random shuffle eps

                if (dbg) {
                    yield.log(*dbg, " Bep5Http: DHT BEP5 lookup result ec:", ec.message(),
                            " eps:", eps);
                }

                assert(ec != asio::error::operation_aborted || cancel);

                if (tried) {
                    eps.erase(*tried);
                    if (dbg) {
                        yield.log(*dbg, " Bep5Http: Removed alredy tried ep:", *tried);
                    }
                }

                return_or_throw_on_error(yield, cancel, ec, Session());
            }

            if (dbg) {
                yield.log(*dbg, " Bep5Http: clients: ", eps, " ec:", ec.message());
            }

            if (cancel) ec = err::operation_aborted;
            if (ec) return or_throw<Session>(yield, ec);

            auto gen = make_connection_generator(eps, dbg);

            while (auto opt_con = gen->async_get_value(cancel, yield[ec])) {
                assert(!cancel || ec == err::operation_aborted);
                if (cancel) ec = err::operation_aborted;
                if (ec == err::operation_aborted) return or_throw<Session>(yield, ec);
                if (ec) continue;

                if (dbg) {
                    yield.log(*dbg, " Bep5Http: Connect to clients done, ec:", ec.message(),
                        " chosen ep:", opt_con->second, "; fetching...");
                }

                auto session = load_from_connection(key, opt_con->first, cancel, yield[ec]);
                auto& hdr = session.response_header();

                if (dbg) {
                    yield.log(*dbg, " Bep5Http: fetch done,",
                        " ec:", ec.message(), " result:", hdr.result());
                }

                assert(!cancel || ec == err::operation_aborted);

                if (cancel) return or_throw<Session>(yield, err::operation_aborted);

                if (ec || hdr.result() == http::status::not_found) {
                    continue;
                }

                // We found the entry
                // TODO: Check its age, store it if it's too old but keep trying
                // other peers.
                peer_cache[dht_group] = opt_con->second;
                return session;
            }
        }

        if (dbg) {
            yield.log(*dbg, " Bep5Http: done cancel:", bool(cancel));
        }


        if (cancel) return or_throw<Session>(yield, asio::error::operation_aborted);
        return or_throw<Session>(yield, err::not_found);
    }

    Session load_from_local( const std::string& key
                           , Cancel cancel
                           , Yield yield)
    {
        sys::error_code ec;
        auto rr = http_store->reader(key, ec);
        if (ec) return or_throw<Session>(yield, ec);
        auto rs = Session::create(move(rr), cancel, yield[ec]);
        assert(!cancel || ec == asio::error::operation_aborted);
        if (!ec) rs.response_header().set( http_::response_source_hdr  // for agent
                                         , http_::response_source_hdr_local_cache);
        return or_throw(yield, ec, move(rs));
    }

    template<class Con>
    Session load_from_connection( const string& key
                                , Con& con
                                , Cancel cancel
                                , Yield yield)
    {
        auto uri = uri_from_key(key);
        http::request<http::string_body> rq{http::verb::get, uri, 11 /* version */};
        rq.set(http::field::host, "dummy_host");
        rq.set(http_::protocol_version_hdr, http_::protocol_version_hdr_current);
        rq.set(http::field::user_agent, "Ouinet.Bep5.Client");

        auto cancelled2 = cancel.connect([&] { con.close(); });

        sys::error_code ec;
        http::async_write(con, rq, yield[ec]);

        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<Session>(yield, ec);

        Session::reader_uptr vfy_reader = make_unique<cache::VerifyingReader>(move(con), cache_pk);
        auto session = Session::create(move(vfy_reader), cancel, yield[ec]);

        assert(!cancel || ec == asio::error::operation_aborted);

        if ( !ec
            && !util::http_proto_version_check_trusted(session.response_header(), newest_proto_seen))
            // The client expects an injection belonging to a supported protocol version,
            // otherwise we just discard this copy.
            ec = asio::error::not_found;

        if (!ec) session.response_header().set( http_::response_source_hdr  // for agent
                                              , http_::response_source_hdr_dist_cache);
        return or_throw(yield, ec, move(session));
    }

    GenericStream connect( udp::endpoint ep
                         , Cancel cancel
                         , asio::yield_context yield)
    {
        sys::error_code ec;
        auto opt_m = choose_multiplexer_for(ep);
        assert(opt_m);
        asio_utp::socket s(ex);
        s.bind(*opt_m, ec);
        if (ec) return or_throw<GenericStream>(yield, ec);
        auto c = cancel.connect([&] { s.close(); });
        bool timed_out = false;
        WatchDog wd(ex, chrono::seconds(30), [&] { timed_out = true; cancel(); });
        s.async_connect(ep, yield[ec]);
        if (timed_out) return or_throw<GenericStream>(yield, asio::error::timed_out);
        if (ec || cancel) return or_throw<GenericStream>(yield, ec);
        return GenericStream(move(s));
    }

    unique_ptr<util::AsyncGenerator<pair<GenericStream, udp::endpoint>>>
    make_connection_generator(set<udp::endpoint> eps, boost::optional<uint32_t> dbg)
    {
        using Ret = util::AsyncGenerator<pair<GenericStream, udp::endpoint>>;

        return make_unique<Ret>(ex,
        [&, lc = lifetime_cancel, eps = move(eps), dbg]
        (auto& q, auto c, auto y) mutable {
            auto cn = lc.connect([&] { c(); });

            WaitCondition wc(ex);
            set<udp::endpoint> our_endpoints = dht->wan_endpoints();

            for (auto& ep : eps) {
                if (bt::is_martian(ep)) continue;
                if (our_endpoints.count(ep)) continue;

                asio::spawn(ex, [&, ep, lock = wc.lock()] (auto y) {
                    TRACK_HANDLER();
                    sys::error_code ec;
                    if (dbg) {
                        std::cerr << *dbg << " Bep5Http: connecting to: " << ep << "\n";
                    }
                    auto s = this->connect(ep, c, y[ec]);
                    if (dbg) {
                        std::cerr << *dbg << " Bep5Http: done connecting to: " << ep << ": "
                            << " ec:" << ec.message() << " c:" << bool(c) << "\n";
                    }
                    if (ec || c) return;
                    q.push_back(make_pair(move(s), ep));
                });
            }

            sys::error_code ec;
            wc.wait(y[ec]);

            if (c) return or_throw(y, asio::error::operation_aborted);
        });
    }

    static bool same_ipv(const udp::endpoint& ep1, const udp::endpoint& ep2)
    {
        return ep1.address().is_v4() == ep2.address().is_v4();
    }

    boost::optional<asio_utp::udp_multiplexer>
    choose_multiplexer_for(const udp::endpoint& ep)
    {
        auto eps = dht->local_endpoints();

        for (auto& e : eps) {
            if (same_ipv(ep, e)) {
                asio_utp::udp_multiplexer m(ex);
                sys::error_code ec;
                m.bind(e, ec);
                assert(!ec);
                return m;
            }
        }

        return boost::none;
    }

    void store( const std::string& key
              , const std::string& dht_group
              , http_response::AbstractReader& r
              , Cancel cancel
              , asio::yield_context yield)
    {
        sys::error_code ec;
        cache::KeepSignedReader fr(r);
        http_store->store(key, fr, cancel, yield[ec]);
        if (ec) return or_throw(yield, ec);

        _dht_groups->add(dht_group, key, cancel, yield[ec]);
        if (ec) return or_throw(yield, ec);

        announcer.add(compute_swarm_name(dht_group));
    }

    http::response_header<>
    read_response_header( http_response::AbstractReader& reader
                        , asio::yield_context yield)
    {
        Cancel lc(lifetime_cancel);

        sys::error_code ec;
        auto part = reader.async_read_part(lc, yield[ec]);
        if (!ec && !part)
            ec = sys::errc::make_error_code(sys::errc::no_message);
        return_or_throw_on_error(yield, lc, ec, http::response_header<>());
        auto head = part->as_head(); assert(head);
        return *head;
    }

    // Return maximum if not available.
    boost::posix_time::time_duration
    cache_entry_age(const http::response_header<>& head)
    {
        using ssecs = std::chrono::seconds;
        using bsecs = boost::posix_time::seconds;

        static auto max_age = bsecs(ssecs::max().count());

        auto ts_sv = util::http_injection_ts(head);
        if (ts_sv.empty()) return max_age;  // missing header or field
        auto ts_o = parse::number<ssecs::rep>(ts_sv);
        if (!ts_o) return max_age;  // malformed creation time stamp
        auto now = ssecs(std::time(nullptr));  // as done by injector
        auto age = now - ssecs(*ts_o);
        return bsecs(age.count());
    }

    inline
    void unpublish_cache_entry(const std::string& key)
    {
        auto empty_groups = _dht_groups->remove(key);
        for (const auto& eg : empty_groups) announcer.remove(eg);
    }

    // Return whether the entry should be kept in storage.
    bool keep_cache_entry(cache::reader_uptr rr, asio::yield_context yield)
    {
        // This should be available to
        // allow removing keys of entries to be evicted.
        assert(_dht_groups);

        sys::error_code ec;

        auto hdr = read_response_header(*rr, yield[ec]);
        if (ec) return or_throw<bool>(yield, ec);

        if (hdr[http_::protocol_version_hdr] != http_::protocol_version_hdr_current) {
            LOG_WARN( "Bep5HTTP: Cached response contains an invalid "
                    , http_::protocol_version_hdr
                    , " header field; removing");
            return false;
        }

        auto key = hdr[http_::response_uri_hdr];
        if (key.empty()) {
            LOG_WARN( "Bep5HTTP: Cached response does not contain a "
                    , http_::response_uri_hdr
                    , " header field; removing");
            return false;
        }

        auto age = cache_entry_age(hdr);
        if (age > max_cached_age) {
            LOG_DEBUG( "Bep5HTTP: Cached response is too old; removing: "
                     , age, " > ", max_cached_age
                     , "; uri=", key );
            unpublish_cache_entry(key.to_string());
            return false;
        }

        return true;
    }

    void announce_stored_data(asio::yield_context y)
    {
        Cancel cancel(lifetime_cancel);

        sys::error_code e;
        _dht_groups = DhtGroups::load(cache_dir/"dht_groups", ex, cancel, y[e]);

        if (cancel) e = asio::error::operation_aborted;
        if (e) return or_throw(y, e);

        http_store->for_each([&] (auto rr, auto yield) {
            return keep_cache_entry(std::move(rr), yield);
        }, cancel, y[e]);
        if (e) return or_throw(y, e);

        for (auto dht_group : _dht_groups->groups()) {
            announcer.add(compute_swarm_name(dht_group));
        }
    }

    void stop() {
        lifetime_cancel();
        local_peer_discovery.stop();
    }

    unsigned get_newest_proto_version() const {
        return newest_proto_seen;
    }

    void set_log_level(log_level_t l) {
        cerr << "Setting Bep5Http Cache log level to " << l << "\n";
        log_level = l;
        announcer.set_log_level(l);
    }

    log_level_t get_log_level() const { return log_level; }
};

/* static */
std::unique_ptr<Client>
Client::build( shared_ptr<bt::MainlineDht> dht
             , util::Ed25519PublicKey cache_pk
             , fs::path cache_dir
             , boost::posix_time::time_duration max_cached_age
             , log_level_t log_level
             , asio::yield_context yield)
{
    using ClientPtr = unique_ptr<Client>;

    sys::error_code ec;

    auto old_store_dir = cache_dir / "data";  // v0 store
    if (is_directory(old_store_dir)) {
        LOG_INFO("Removing obsolete HTTP store...");
        fs::remove_all(old_store_dir, ec);
        if (ec) LOG_ERROR("Removing obsolete HTTP store: failed; ec:", ec.message());
        else LOG_INFO("Removing obsolete HTTP store: done");
        ec = {};
    }

    auto store_dir = cache_dir / "data-v1";
    fs::create_directories(store_dir, ec);
    if (ec) return or_throw<ClientPtr>(yield, ec);
    auto http_store = make_unique<cache::HttpStoreV1>(
        move(store_dir), dht->get_executor());

    unique_ptr<Impl> impl(new Impl( move(dht)
                                  , cache_pk, move(cache_dir)
                                  , move(http_store), max_cached_age
                                  , log_level));

    impl->announce_stored_data(yield[ec]);
    if (ec) return or_throw<ClientPtr>(yield, ec);
    impl->gc.start();

    return unique_ptr<Client>(new Client(move(impl)));
}

Client::Client(unique_ptr<Impl> impl)
    : _impl(move(impl))
{}

Session Client::load(const std::string& key, const std::string& dht_group, Cancel cancel, Yield yield)
{
    return _impl->load(key, dht_group, cancel, yield);
}

void Client::store( const std::string& key
                  , const std::string& dht_group
                  , http_response::AbstractReader& r
                  , Cancel cancel
                  , asio::yield_context yield)
{
    _impl->store(key, dht_group, r, cancel, yield);
}

void Client::serve_local( const http::request<http::empty_body>& req
                        , GenericStream& sink
                        , Cancel& cancel
                        , asio::yield_context yield)
{
    _impl->serve_local(req, sink, cancel, yield);
}

std::size_t Client::local_size( Cancel cancel
                              , asio::yield_context yield) const
{
    return _impl->local_size(cancel, yield);
}

void Client::local_purge( Cancel cancel
                        , asio::yield_context yield)
{
    _impl->local_purge(cancel, yield);
}

unsigned Client::get_newest_proto_version() const
{
    return _impl->get_newest_proto_version();
}

void Client::set_log_level(log_level_t l)
{
    _impl->set_log_level(l);
}

log_level_t Client::get_log_level() const
{
    return _impl->get_log_level();
}

Client::~Client()
{
    _impl->stop();
}
