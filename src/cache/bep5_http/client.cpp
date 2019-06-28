#include "client.h"
#include "../../util/sha1.h"
#include "../../util/bytes.h"
#include "../../util/file_io.h"
#include "../../bittorrent/dht.h"
#include "../../bittorrent/bep5_announcer.h"
#include "../../bittorrent/is_martian.h"
#include "../../ouiservice/utp.h"
#include "../../ouiservice/bep5.h"
#include "../../logger.h"
#include "../../async_sleep.h"
#include "../../constants.h"
#include <map>
#include <set>
#include <network/uri.hpp>

using namespace std;
using namespace ouinet;
using namespace cache::bep5_http;
using udp = asio::ip::udp;
using tcp = asio::ip::tcp;

namespace fs = boost::filesystem;
namespace bt = bittorrent;

struct Client::Impl {
    asio::io_service& ios;
    shared_ptr<bt::MainlineDht> dht;
    fs::path cache_dir;
    Cancel cancel;
    map<string, unique_ptr<bt::Bep5Announcer>> swarm_announcers;
    // TODO: Should be Lru
    map<string, udp::endpoint> peer_cache;

    Impl(shared_ptr<bt::MainlineDht> dht_, fs::path cache_dir)
        : ios(dht_->get_io_service())
        , dht(move(dht_))
        , cache_dir(move(cache_dir))
    {
        start_accepting();
    }

    void start_accepting()
    {
        for (auto ep : dht->local_endpoints()) {
            asio::spawn(ios, [&, ep] (asio::yield_context yield) {
                Cancel c(cancel);
                sys::error_code ec;
                start_accepting_on(ep, c, yield[ec]);
            });
        }
    }

    void start_accepting_on( udp::endpoint ep
                           , Cancel& cancel
                           , asio::yield_context yield)
    {
        auto srv = make_unique<ouiservice::UtpOuiServiceServer>(ios, ep);

        sys::error_code ec;
        srv->start_listen(yield[ec]);

        if (cancel) return;

        if (ec) {
            LOG_ERROR("Bep5Http: Failed to start listening on uTP: ", ep);
            return;
        }

        while (true) {
            sys::error_code ec;

            GenericStream con = srv->accept(yield[ec]);

            if (cancel) return;
            if (ec == asio::error::operation_aborted) return;
            if (ec) {
                LOG_WARN("Bep5Http: Failure to accept:", ec.message());
                async_sleep(ios, 200ms, cancel, yield);
                continue;
            }

            asio::spawn(ios, [&, con = move(con)]
                             (asio::yield_context yield) mutable {
                Cancel c(cancel);
                sys::error_code ec;
                serve(con, c, yield[ec]);
            });
        }
    }

    void serve(GenericStream& con, Cancel& cancel, asio::yield_context yield)
    {
        sys::error_code ec;

        http::request<http::empty_body> req;
        beast::flat_buffer buffer;
        http::async_read(con, buffer, req, yield[ec]);

        if (ec || cancel) return;

        string key = key_from_http_req(req);

        auto path = path_from_key(key);

        auto file = util::file_io::open_readonly(ios, path, ec);

        if (ec) {
            return handle_not_found(con, req, yield[ec]);
        }

        flush_from_to(file, con, cancel, yield[ec]);

        return or_throw(yield, ec);
    }

    void handle_not_found( GenericStream& con
                         , const http::request<http::empty_body>& req
                         , asio::yield_context yield)
    {
        http::response<http::empty_body>
            res{http::status::not_found, req.version()};

        res.set(http::field::server, OUINET_CLIENT_SERVER_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.prepare_payload();

        http::async_write(con, res, yield);
    }

    string get_host(const string& uri_s)
    {
        network::uri uri(uri_s);
        return uri.host().to_string();
    }

    void load( const std::string& key
             , GenericStream& sink
             , Cancel cancel
             , Yield yield)
    {
        auto host = get_host(key);
        auto canceled = this->cancel.connect([&] { cancel(); });

        auto peer_i = peer_cache.find(host);

        if (peer_i != peer_cache.end()) {
            sys::error_code ec;
            auto con = connect(peer_i->second, cancel, yield[ec]);

            if (cancel) return or_throw(yield, asio::error::operation_aborted);
            if (!ec) {
                load(key, con, sink, cancel, yield[ec]);

                if (!ec) {
                    return;
                }

                peer_cache.erase(host);
            }
            else {
                peer_cache.erase(host);
            }
        }

        sys::error_code ec;

        auto endpoints = dht->tracker_get_peers(util::sha1(key), cancel, yield[ec]);

        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw(yield, ec);

        udp::endpoint con_ep;
        auto con = connect(tcp_to_udp(endpoints), con_ep, cancel, yield[ec]);

        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw(yield, ec);

        peer_cache[host] = con_ep;

        load(key, con, sink, cancel, yield[ec]);

        return or_throw(yield, ec);
    }

    template<class Con>
    void load( const std::string& key
               , Con& con
               , GenericStream& sink
               , Cancel cancel
               , Yield yield)
    {
        http::request<http::string_body> rq{http::verb::get, key, 11 /* version */};
        rq.set(http::field::host, "dummy_host");
        rq.set(http::field::user_agent, "Ouinet.Bep5.Client");

        auto cancelled2 = cancel.connect([&] { con.close(); });

        sys::error_code ec;
        http::async_write(con, rq, yield[ec]);

        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw(yield, ec);

        flush_from_to(con, sink, cancel, yield);
    }

    static set<udp::endpoint> tcp_to_udp(const set<tcp::endpoint>& eps)
    {
        set<udp::endpoint> ret;
        for (auto& ep : eps) { ret.insert({ep.address(), ep.port()}); }
        return ret;
    }

    GenericStream connect( udp::endpoint ep
                         , Cancel& cancel
                         , asio::yield_context yield)
    {
        sys::error_code ec;
        auto opt_m = choose_multiplexer_for(ep);
        assert(opt_m);
        asio_utp::socket s(ios);
        s.bind(*opt_m, ec);
        if (ec) return or_throw<GenericStream>(yield, ec);
        auto c = cancel.connect([&] { s.close(); });
        s.async_connect(ep, yield[ec]);
        if (ec || cancel) return or_throw<GenericStream>(yield, ec);
        return GenericStream(move(s));
    }

    GenericStream connect( set<udp::endpoint> eps
                         , udp::endpoint& ret_ep
                         , Cancel& cancel
                         , asio::yield_context yield)
    {
        boost::optional<GenericStream> retval;

        WaitCondition wc(ios);

        Cancel local_cancel(cancel);

        set<udp::endpoint> our_endpoints = dht->wan_endpoints();

        for (auto& ep : eps) {
            if (bt::is_martian(ep)) continue;
            if (our_endpoints.count(ep)) continue;

            asio::spawn(ios, [&, ep, lock = wc.lock()] (asio::yield_context yield) {
                sys::error_code ec;
                auto s = connect(ep, local_cancel, yield[ec]);
                if (ec || local_cancel) return;
                ret_ep = ep;
                retval = move(s);
                local_cancel();
            });
        }

        sys::error_code ec;
        wc.wait(cancel, yield[ec]);

        if (!retval) ec = asio::error::host_unreachable;
        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<GenericStream>(yield, ec);

        return move(*retval);
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
                asio_utp::udp_multiplexer m(ios);
                sys::error_code ec;
                m.bind(e, ec);
                assert(!ec);
                return m;
            }
        }

        return boost::none;
    }

    void store( const std::string& key
              , const http::response_header<>& rs_hdr
              , GenericStream& rs_body
              , Cancel cancel
              , asio::yield_context yield)
    {
        sys::error_code ec;

        auto infohash = util::sha1(key);
        auto path = path_from_infohash(infohash);
        auto file = open_or_create(path, ec);
        if (ec) return or_throw(yield, ec);

        http::response<http::empty_body> rs_hdr_msg{rs_hdr};
        http::response_serializer<http::empty_body> rs_hdr_s(rs_hdr_msg);

        http::async_write_header(file, rs_hdr_s, yield[ec]);

        if (ec) {
            try_remove(path);
            return or_throw(yield, ec);
        }

        flush_from_to(rs_body, file, cancel, yield[ec]);

        if (ec) {
            try_remove(path);
            return or_throw(yield, ec);
        }

        announce(key);
    }

    void announce(string key)
    {
        auto infohash = util::sha1(key);
        auto res = swarm_announcers.emplace(move(key), nullptr);

        if (res.second /* inserted? */) {
            res.first->second.reset(new bt::Bep5Announcer(infohash, dht));
        }
    }

    template<class Stream>
    http::response_header<>
    read_response_header(Stream& stream, asio::yield_context yield)
    {
        sys::error_code ec;
        beast::flat_buffer buffer;
        http::response_parser<http::empty_body> parser;
        http::async_read_header(stream, buffer, parser, yield[ec]);

        if (cancel) ec = asio::error::operation_aborted;
        if (ec) return or_throw<http::response_header<>>(yield, ec);

        return parser.release();
    }

    void announce_stored_data(asio::yield_context yield)
    {
        for (auto& p : fs::directory_iterator(data_dir())) {
            if (!fs::is_regular_file(p)) continue;
            sys::error_code ec;

            auto f = util::file_io::open_readonly(ios, p, ec);
            if (ec == asio::error::operation_aborted) return;
            if (ec) { try_remove(p); continue; }

            auto hdr = read_response_header(f, yield[ec]);
            if (ec == asio::error::operation_aborted) return;
            if (ec) { try_remove(p); continue; }

            auto key = hdr[http_::response_injection_key];

            if (key.empty()) { try_remove(p); continue; }

            LOG_DEBUG("Announcing stored: ", key);
            announce(key.to_string());
        }
    }

    void try_remove(const fs::path& path)
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
        return path_from_infohash(util::sha1(key));
    }

    fs::path path_from_infohash(const bt::NodeID& infohash)
    {
        return data_dir()/infohash.to_hex();
    }

    asio::posix::stream_descriptor open_or_create( const fs::path& path
                                                 , sys::error_code& ec)
    {
        auto file = util::file_io::open_or_create(ios, path, ec);

        if (ec) return file;

        util::file_io::truncate(file, 0, ec);

        return file;
    }

    template<class Source, class Sink>
    size_t flush_from_to( Source& source
                        , Sink& sink
                        , Cancel& cancel
                        , asio::yield_context yield)
    {
        sys::error_code ec;
        std::array<uint8_t, 1 << 14> data;

        size_t s = 0;

        for (;;) {
            size_t length = source.async_read_some(asio::buffer(data), yield[ec]);
            if (ec || cancel) break;

            asio::async_write(sink, asio::buffer(data, length), yield[ec]);
            if (ec || cancel) break;

            s += length;
        }

        if (ec == asio::error::eof) {
            ec = sys::error_code();
        }

        return or_throw(yield, ec, s);
    }

    void stop() {
        cancel();
    }
};

/* static */
std::unique_ptr<Client>
Client::build( shared_ptr<bt::MainlineDht> dht
             , fs::path cache_dir
             , asio::yield_context yield)
{
    using ClientPtr = unique_ptr<Client>;

    sys::error_code ec;

    fs::create_directories(cache_dir/"data", ec);

    if (ec) return or_throw<ClientPtr>(yield, ec);

    unique_ptr<Impl> impl(new Impl(move(dht), move(cache_dir)));

    impl->announce_stored_data(yield[ec]);

    if (ec) return or_throw<ClientPtr>(yield, ec);

    return unique_ptr<Client>(new Client(move(impl)));
}

Client::Client(unique_ptr<Impl> impl)
    : _impl(move(impl))
{}

void Client::load( const std::string& key
                 , GenericStream& sink
                 , Cancel cancel
                 , Yield yield)
{
    _impl->load(key, sink, cancel, yield);
}

void Client::store( const std::string& key
                  , const http::response_header<>& rs_hdr
                  , GenericStream& rs_body
                  , Cancel cancel
                  , asio::yield_context yield)
{
    _impl->store(key, rs_hdr, rs_body, cancel, yield);
}

Client::~Client()
{
    _impl->stop();
}
