#include "client.h"
#include "announcer.h"
#include "dht_lookup.h"
#include "local_peer_discovery.h"
#include "../http_sign.h"
#include "../http_store.h"
#include "../../http_util.h"
#include "../../util/wait_condition.h"
#include "../../util/set_io.h"
#include "../../util/async_generator.h"
#include "../../util/connected_pair.h"
#include "../../util/lru_cache.h"
#include "../../bittorrent/dht.h"
#include "../../bittorrent/is_martian.h"
#include "../../ouiservice/utp.h"
#include "../../logger.h"
#include "../../async_sleep.h"
#include "../../constants.h"
#include "../../session.h"
#include <map>

using namespace std;
using namespace ouinet;
using namespace cache::bep5_http;
using udp = asio::ip::udp;

namespace fs = boost::filesystem;
namespace bt = bittorrent;

struct Client::Impl {
    // The newest protocol version number seen in a trusted exchange
    // (i.e. from injector-signed cached content).
    unsigned newest_proto_seen = http_::protocol_version_current;

    asio::executor ex;
    shared_ptr<bt::MainlineDht> dht;
    util::Ed25519PublicKey cache_pk;
    fs::path cache_dir;
    unique_ptr<cache::AbstractHttpStore> http_store;
    Cancel lifetime_cancel;
    Announcer announcer;
    map<string, udp::endpoint> peer_cache;
    util::LruCache<bt::NodeID, unique_ptr<DhtLookup>> dht_lookups;
    log_level_t log_level = INFO;
    LocalPeerDiscovery local_peer_discovery;
    uint32_t debug_next_load_nr = 0;


    bool log_debug() const { return log_level <= DEBUG; }
    bool log_info()  const { return log_level <= INFO; }

    Impl( shared_ptr<bt::MainlineDht> dht_
        , util::Ed25519PublicKey& cache_pk
        , fs::path cache_dir
        , unique_ptr<cache::AbstractHttpStore> http_store
        , log_level_t log_level)
        : ex(dht_->get_executor())
        , dht(move(dht_))
        , cache_pk(cache_pk)
        , cache_dir(move(cache_dir))
        , http_store(move(http_store))
        , announcer(dht, log_level)
        , dht_lookups(256)
        , log_level(log_level)
        , local_peer_discovery(ex, dht->local_endpoints())
    {}

    // "http(s)://www.foo.org/bar/baz" -> "www.foo.org"
    boost::optional<string> dht_key(const string& s)
    {
        return get_host(s);
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

    boost::optional<string> get_host(const string& uri_s)
    {
#if 0
        // This code sometime throws an exception.
        network::uri uri(uri_s);
        return uri.host().to_string();
#else
        beast::string_view s(uri_s);

        if (s.starts_with("http://")) {
            s.remove_prefix(7);
        } else if (s.starts_with("https://")) {
            s.remove_prefix(8);
        }

        auto p = s.find('/');

        if (p == s.npos) return boost::none;

        s = s.substr(0, p);

        return s.to_string();
#endif
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

    Session load(const std::string& key, Cancel cancel, Yield yield)
    {
        boost::optional<decltype(debug_next_load_nr)> dbg;

        if (log_debug()) {
            dbg = debug_next_load_nr++;
        }

        namespace err = asio::error;

        auto opt_host = get_host(key);

        if (!opt_host) {
            return or_throw<Session>(yield, err::invalid_argument);
        }

        auto& host = *opt_host;

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
                auto peer_i = peer_cache.find(host);
                if (peer_i == peer_cache.end()) continue;
                auto ep = peer_i->second;
                if (dbg) {
                    yield.log(*dbg, " Bep5Http: using cached endpoint first:", ep);
                }
                eps = {ep};
                tried = ep;
            }
            else if (do_try == dht_peers) {
                bt::NodeID infohash = util::sha1_digest(host);

                if (dbg) {
                    yield.log(*dbg, " Bep5Http: DHT lookup:");
                    yield.log(*dbg, "     key:     ", key);
                    yield.log(*dbg, "     dht_key: ", host);
                    yield.log(*dbg, "     infohash:", infohash);
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
                peer_cache[host] = opt_con->second;
                return session;
            }
        }

        if (dbg) {
            yield.log(*dbg, " Bep5Http: done cancel:", bool(cancel));
        }


        if (cancel) return or_throw<Session>(yield, asio::error::operation_aborted);
        return or_throw<Session>(yield, err::not_found);
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
              , http_response::AbstractReader& r
              , Cancel cancel
              , asio::yield_context yield)
    {
        auto dk = dht_key(key);
        if (!dk) return or_throw(yield, asio::error::invalid_argument);

        sys::error_code ec;
        http_store->store(key, r, cancel, yield[ec]);
        if (ec) return or_throw(yield, ec);

        announcer.add(*dk);
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

    void announce_stored_data(asio::yield_context yield)
    {
        for (auto& p : fs::directory_iterator(data_dir())) {
            if (!fs::is_regular_file(p)) continue;
            sys::error_code ec;

            auto rr = cache::http_store_reader_v0(p, ex, ec);
            if (ec == asio::error::operation_aborted) return;
            if (ec) {
                LOG_WARN("Bep5HTTP: Failed to open cached file ", p
                        , " ec:", ec.message());
                try_remove(p); continue;
            }

            auto hdr = read_response_header(*rr, yield[ec]);
            if (ec == asio::error::operation_aborted) return;
            if (ec) {
                LOG_WARN("Bep5HTTP: Failed read cached file ", p
                        , " ec:", ec.message());
                try_remove(p); continue;
            }

            if (hdr[http_::protocol_version_hdr] != http_::protocol_version_hdr_current) {
                LOG_WARN("Bep5HTTP: Cached file ", p
                        , " contains an invalid ", http_::protocol_version_hdr
                        , " header field (removing the file)");
                try_remove(p); continue;
            }

            auto key = hdr[http_::response_uri_hdr];

            if (key.empty()) {
                LOG_WARN("Bep5HTTP: Cached file ", p
                        , " does not contain ", http_::response_uri_hdr
                        , " header field (removing the file)");
                try_remove(p); continue;
            }

            if (auto opt_k = dht_key(key.to_string())) {
                announcer.add(*opt_k);
            }
        }
    }

    static void try_remove(const fs::path& path)
    {
        sys::error_code ec_ignored;
        fs::remove(path, ec_ignored);
    }

    fs::path data_dir() const
    {
        return cache_dir/"data";
    }

    fs::path path_from_key(const std::string& key)
    {
        return path_from_infohash(util::sha1_digest(key));
    }

    fs::path path_from_infohash(const bt::NodeID& infohash)
    {
        return data_dir()/infohash.to_hex();
    }

    void stop() {
        lifetime_cancel();
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
             , log_level_t log_level
             , asio::yield_context yield)
{
    using ClientPtr = unique_ptr<Client>;

    sys::error_code ec;

    auto store_dir = cache_dir / "data"/*"-v0"*/;
    fs::create_directories(store_dir, ec);
    if (ec) return or_throw<ClientPtr>(yield, ec);
    auto http_store = make_unique<cache::HttpStoreV0>(
        move(store_dir), dht->get_executor());

    unique_ptr<Impl> impl(new Impl( move(dht)
                                  , cache_pk, move(cache_dir)
                                  , move(http_store)
                                  , log_level));

    impl->announce_stored_data(yield[ec]);

    if (ec) return or_throw<ClientPtr>(yield, ec);

    return unique_ptr<Client>(new Client(move(impl)));
}

Client::Client(unique_ptr<Impl> impl)
    : _impl(move(impl))
{}

Session Client::load(const std::string& key, Cancel cancel, Yield yield)
{
    return _impl->load(key, cancel, yield);
}

void Client::store( const std::string& key
                  , http_response::AbstractReader& r
                  , Cancel cancel
                  , asio::yield_context yield)
{
    _impl->store(key, r, cancel, yield);
}

void Client::serve_local( const http::request<http::empty_body>& req
                        , GenericStream& sink
                        , Cancel& cancel
                        , asio::yield_context yield)
{
    _impl->serve_local(req, sink, cancel, yield);
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
