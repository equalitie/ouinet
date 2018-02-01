#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <iostream>
#include <fstream>

#include <ipfs_cache/client.h>
#include <ipfs_cache/error.h>

#include <gnunet_channels/channel.h>
#include <gnunet_channels/service.h>

#include <i2poui.h>

#include "namespaces.h"
#include "connect_to_host.h"
#include "fetch_http_page.h"
#include "client_front_end.h"
#include "generic_connection.h"
#include "util.h"
#include "result.h"
#include "blocker.h"
#include "async_sleep.h"
#include "increase_open_file_limit.h"
#include "endpoint.h"
#include "cache_control.h"
#include "or_throw.h"
#include "request_routing.h"

using namespace std;
using namespace ouinet;

namespace fs = boost::filesystem;
namespace posix_time = boost::posix_time;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::string_body>;
using Response    = http::response<http::dynamic_body>;

static fs::path REPO_ROOT;
static const fs::path OUINET_CONF_FILE = "ouinet-client.conf";
static posix_time::time_duration MAX_CACHED_AGE = posix_time::hours(7*24);  // one week

//------------------------------------------------------------------------------
#define ASYNC_DEBUG(code, ...) [&] () mutable {\
    auto task = client.front_end.notify_task(util::str(__VA_ARGS__));\
    return code;\
}()

//------------------------------------------------------------------------------
struct Client {
    struct I2P {
        i2poui::Service service;
        i2poui::Connector connector;
    };

    asio::io_service& ios;
    Endpoint injector_ep;
    unique_ptr<gnunet_channels::Service> gnunet_service;
    unique_ptr<I2P> i2p;
    unique_ptr<ipfs_cache::Client> ipfs_cache;

    ClientFrontEnd front_end;

    Client(asio::io_service& ios, const Endpoint& injector_ep)
        : ios(ios)
        , injector_ep(injector_ep)
    {}
};

//------------------------------------------------------------------------------
static
void handle_bad_request( GenericConnection& con
                       , const Request& req
                       , string message
                       , asio::yield_context yield)
{
    http::response<http::string_body> res{http::status::bad_request, req.version()};

    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/html");
    res.keep_alive(req.keep_alive());
    res.body() = message;
    res.prepare_payload();

    sys::error_code ec;
    http::async_write(con, res, yield[ec]);
}

//------------------------------------------------------------------------------
static
void forward(GenericConnection& in, GenericConnection& out, asio::yield_context yield)
{
    sys::error_code ec;
    array<uint8_t, 2048> data;

    for (;;) {
        size_t length = in.async_read_some(asio::buffer(data), yield[ec]);
        if (ec) break;

        asio::async_write(out, asio::buffer(data, length), yield[ec]);
        if (ec) break;
    }
}

//------------------------------------------------------------------------------
static
void handle_connect_request( GenericConnection& client_c
                           , const Request& req
                           , asio::yield_context yield)
{
    sys::error_code ec;
    asio::io_service& ios = client_c.get_io_service();

    auto origin_c = connect_to_host(ios, req["host"], ec, yield);
    if (ec) return fail(ec, "connect");

    // Send the client an OK message indicating that the tunnel
    // has been established. TODO: Reply with an error otherwise.
    http::response<http::empty_body> res{http::status::ok, req.version()};
    http::async_write(client_c, res, yield[ec]);

    if (ec) return fail(ec, "sending connect response");

    Blocker blocker(ios);

    asio::spawn
        ( yield
        , [&, b = blocker.make_block()](asio::yield_context yield) {
              forward(client_c, origin_c, yield);
          });

    asio::spawn
        ( yield
        , [&, b = blocker.make_block()](asio::yield_context yield) {
              forward(origin_c, client_c, yield);
          });

    blocker.wait(yield);
}

//------------------------------------------------------------------------------
static
Result<GenericConnection>
connect_to_injector(Client& client, asio::yield_context yield)
{
    struct Visitor {
        using Ret = Result<GenericConnection>;

        sys::error_code ec;
        Client& client;
        asio::yield_context yield;

        Visitor(Client& client, asio::yield_context yield)
            : client(client), yield(yield) {}

        Ret operator()(const tcp::endpoint& ep) {
            tcp::socket socket(client.ios);
            socket.async_connect(ep, yield[ec]);

            if (ec) return ec;
            return GenericConnection(move(socket));
        }

        Ret operator()(const GnunetEndpoint& ep) {
            using Channel = gnunet_channels::Channel;

            if (!client.gnunet_service) {
                return Ret::make_error(asio::error::no_protocol_option);
            }

            Channel ch(*client.gnunet_service);
            ch.connect(ep.host, ep.port, yield[ec]);

            if (ec) return ec;
            return GenericConnection(move(ch));
        }

        Ret operator()(const I2PEndpoint&) {
            if (!client.i2p) {
                return Ret::make_error(asio::error::no_protocol_option);
            }

            i2poui::Channel ch(client.i2p->service);
            ch.connect(client.i2p->connector, yield[ec]);

            if (ec) return ec;
            return GenericConnection(move(ch));
        }
    };

    Visitor visitor(client, yield);

    return boost::apply_visitor(visitor, client.injector_ep);
}

//------------------------------------------------------------------------------
static
CacheControl::CacheEntry
fetch_from_cache( const Request& request
                , request_route::Config& request_config
                , Client& client
                , asio::yield_context yield)
{
    using CacheEntry = CacheControl::CacheEntry;

    const bool cache_is_disabled
        = !request_config.enable_cache
       || !client.ipfs_cache
       || !client.front_end.is_ipfs_cache_enabled();

    if (cache_is_disabled) {
        return or_throw<CacheControl::CacheEntry>( yield ,
                asio::error::operation_not_supported);
    }

    sys::error_code ec;
    // Get the content from cache
    auto key = request.target();

    auto content = client.ipfs_cache->get_content(key.to_string(), yield[ec]);

    // We need this remapping because CacheControl doesn't know
    // anything about ipfs_cache.
    // TODO: Make ipfs_cache return asio::error::not_found instead.
    if (ec == ipfs_cache::error::key_not_found) {
        ec = asio::error::not_found;
    }

    if (ec) return or_throw<CacheEntry>(yield, ec);

    // If the content does not have a meaningful time stamp,
    // an error should have been reported.
    assert(!content.ts.is_not_a_date_time());

    http::response_parser<Response::body_type> parser;
    parser.eager(true);
    parser.put(asio::buffer(content.data), ec);

    assert(!ec && "Malformed cache entry");

    if (!parser.is_done()) {
        cerr << "------- WARNING: Unfinished message in cache --------" << endl;
        cerr << request << parser.get() << endl;
        cerr << "-----------------------------------------------------" << endl;
        ec = asio::error::not_found;
    }

    if (ec) return or_throw<CacheEntry>(yield, ec);

    return CacheEntry{content.ts, parser.release()};
}

//------------------------------------------------------------------------------
static
Response
fetch_from_origin( const Request& request
                 , request_route::Config& request_config
                 , Client& client
                 , asio::yield_context yield)
{
    using namespace asio::error;
    using request_route::responder;

    sys::error_code last_error = operation_not_supported;

    while (!request_config.responders.empty()) {
        auto r = request_config.responders.front();
        request_config.responders.pop();

        switch (r) {
            case responder::origin: {
                assert(0 && "TODO");
                continue;
            }
            case responder::proxy: {
                assert(0 && "TODO");
                continue;
            }
            case responder::injector: {
                if (!client.front_end.is_injector_proxying_enabled()) {
                    continue;
                }
                auto inj_con = connect_to_injector(client, yield);
                if (inj_con.is_error()) {
                    last_error = inj_con.get_error();
                    continue;
                }
                sys::error_code ec;
                // Forward the request to the injector
                auto res = fetch_http_page(client.ios, *inj_con, request, yield[ec]);
                if (!ec) return res;
                last_error = ec;
                continue;
            }
            case responder::_front_end: {
                return client.front_end.serve( client.injector_ep
                                             , request
                                             , client.ipfs_cache.get());
            }
        }
    }

    return or_throw<Response>(yield, last_error);
}

//------------------------------------------------------------------------------
static
CacheControl build_cache_control( asio::io_service& ios
                                , request_route::Config& request_config
                                , Client& client)
{
    CacheControl cache_control;

    cache_control.fetch_from_cache =
        [&] (const Request& request, asio::yield_context yield) {
            return ASYNC_DEBUG( fetch_from_cache(request, request_config, client, yield)
                              , "Fetch from cache: " , request.target());
        };

    cache_control.fetch_from_origin =
        [&] (const Request& request, asio::yield_context yield) {
            return ASYNC_DEBUG( fetch_from_origin(request, request_config, client, yield)
                              , "Fetch from origin: ", request.target());
        };

    cache_control.max_cached_age(MAX_CACHED_AGE);

    return cache_control;
}

//------------------------------------------------------------------------------
static void serve_request( GenericConnection con
                         , Client& client
                         , asio::yield_context yield)
{
    namespace rr = request_route;
    using rr::responder;

    const rr::Config default_request_config
        { true
        , queue<responder>({responder::injector})};

    rr::Config request_config;

    CacheControl cache_control = build_cache_control( con.get_io_service()
                                                    , request_config
                                                    , client);

    sys::error_code ec;
    beast::flat_buffer buffer;

    // These hard-wired access mechanisms are attempted in order for all normal requests.
    // Expressions/mechanisms to test the request against.
    using Match = pair<const ouinet::reqexpr::reqex, const rr::Config>;

    auto method_getter([](const Request& r) {return r.method_string();});
    auto host_getter([](const Request& r) {return r["Host"];});
    auto target_getter([](const Request& r) {return r.target();});

    const vector<Match> matches({
        Match( reqexpr::from_regex(host_getter, "localhost")
             , {false, queue<responder>({responder::_front_end})} ),
        // Send non-safe HTTP method requests to the origin server
        // NOTE: The cache needs not be disabled as it should know not to
        // fetch requests in these cases.
        Match( reqexpr::from_regex(method_getter, "(HEAD|OPTIONS|TRACE)")
             , {false, queue<responder>({responder::origin})} ),
        // Do not use cache for safe but non-cacheable HTTP method requests
        // NOTE: same as above.
        Match( reqexpr::from_regex(method_getter, "(OPTIONS|TRACE)")
             , {false, queue<responder>({responder::injector, responder::origin})} ),
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example.com/.*")
             , {true, queue<responder>()} ),
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example.net/.*")
             , {true, queue<responder>({responder::injector, responder::origin})} ),
    });

    // Process the different requests that may come over the same connection.
    for (;;) {  // continue for next request; break for no more requests
        Request req;

        // Read the (clear-text) HTTP request
        ASYNC_DEBUG(http::async_read(con, buffer, req, yield[ec]), "Read request");

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "read");

        // Attempt connection to origin for CONNECT requests
        if (req.method() == http::verb::connect) {
            return ASYNC_DEBUG(handle_connect_request(con, req, yield), "Connect");
        }

        request_config = route_choose_config(req, matches, default_request_config);

        auto res = ASYNC_DEBUG( cache_control.fetch(req, yield[ec])
                              , "CacheControl::fetch "
                              , req.target());

        if (ec) {
            cerr << "----- WARNING: Error fetching --------" << endl;
            cerr << "Error Code: " << ec.message() << endl;
            cerr << req << res.base() << endl;
            cerr << "--------------------------------------" << endl;

            // TODO: Better error message.
            ASYNC_DEBUG(handle_bad_request(con, req, "Not cached", yield), "Send error");
            if (req.keep_alive()) continue;
            else return;
        }

        // Forward the response back
        ASYNC_DEBUG(http::async_write(con, res, yield[ec]), "Write response ", req.target());
        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "write");
    }
}

//------------------------------------------------------------------------------
void do_listen( Client& client
              , tcp::endpoint local_endpoint
              , string ipns
              , asio::yield_context yield)
{
    auto& ios = client.ios;

    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(ios);

    acceptor.open(local_endpoint.protocol(), ec);
    if (ec) return fail(ec, "open");

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(local_endpoint, ec);
    if (ec) return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) return fail(ec, "listen");

    if (ipns.size()) {
        ipfs_cache::Client cache(ios, ipns, (REPO_ROOT/"ipfs").native());
        client.ipfs_cache = make_unique<ipfs_cache::Client>(move(cache));
    }

    cout << "Client accepting on " << acceptor.local_endpoint() << endl;

    for(;;)
    {
        tcp::socket socket(ios);
        ASYNC_DEBUG(acceptor.async_accept(socket, yield[ec]), "Accept");
        if(ec) {
            fail(ec, "accept");
            ASYNC_DEBUG(async_sleep(ios, chrono::seconds(1), yield), "Sleep");
        }
        else {
            asio::spawn( ios
                       , [ s = move(socket)
                         , &client
                         ](asio::yield_context yield) mutable {
                             serve_request( GenericConnection(move(s))
                                          , client
                                          , yield);
                         });
        }
    }
}

//------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    namespace po = boost::program_options;

    po::options_description desc("\nOptions");

    desc.add_options()
        ("help", "Produce this help message")
        ("repo", po::value<string>(), "Path to the repository root")
        ("listen-on-tcp", po::value<string>(), "IP:PORT endpoint on which we'll listen")
        ("injector-ep"
         , po::value<string>()
         , "Injector's endpoint (either <IP>:<PORT> or <GNUnet's ID>:<GNUnet's PORT>")
        ("injector-ipns"
         , po::value<string>()->default_value("")
         , "IPNS of the injector's database")
        ("max-cached-age"
         , po::value<int>()->default_value(MAX_CACHED_AGE.total_seconds())
         , "Discard cached content older than this many seconds (0: discard all; -1: discard none)")
        ("open-file-limit"
         , po::value<unsigned int>()
         , "To increase the maximum number of open files")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (!vm.count("repo")) {
        cerr << "The 'repo' argument is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    REPO_ROOT = fs::path(vm["repo"].as<string>());

    if (!fs::exists(REPO_ROOT)) {
        cerr << "Directory " << REPO_ROOT << " does not exist." << endl;
        cerr << desc << endl;
        return 1;
    }

    if (!fs::is_directory(REPO_ROOT)) {
        cerr << "The path " << REPO_ROOT << " is not a directory." << endl;
        cerr << desc << endl;
        return 1;
    }

    fs::path ouinet_conf_path = REPO_ROOT/OUINET_CONF_FILE;

    if (!fs::is_regular_file(ouinet_conf_path)) {
        cerr << "The path " << REPO_ROOT << " does not contain "
             << "the " << OUINET_CONF_FILE << " configuration file." << endl;
        cerr << desc << endl;
        return 1;
    }

    ifstream ouinet_conf(ouinet_conf_path.native());

    po::store(po::parse_config_file(ouinet_conf, desc), vm);
    po::notify(vm);

    if (vm.count("open-file-limit")) {
        increase_open_file_limit(vm["open-file-limit"].as<unsigned int>());
    }

    if (vm.count("max-cached-age")) {
        MAX_CACHED_AGE = boost::posix_time::seconds(vm["max-cached-age"].as<int>());
    }

    if (!vm.count("listen-on-tcp")) {
        cerr << "The parameter 'listen-on-tcp' is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    if (!vm.count("injector-ep")) {
        cerr << "The parameter 'injector-ep' is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    auto const local_ep = util::parse_endpoint(vm["listen-on-tcp"].as<string>());
    auto const injector_ep = *parse_endpoint(vm["injector-ep"].as<string>());

    string ipns;

    if (vm.count("injector-ipns")) {
        ipns = vm["injector-ipns"].as<string>();
    }

    // The io_service is required for all I/O
    asio::io_service ios;

    asio::spawn
        ( ios
        , [&](asio::yield_context yield) {

              Client client(ios, injector_ep);

              if (is_gnunet_endpoint(injector_ep)) {
                  namespace gc = gnunet_channels;

                  string config = (REPO_ROOT/"gnunet"/"peer.conf").native();

                  auto service = make_unique<gc::Service>(config, ios);

                  sys::error_code ec;

                  cout << "Setting up GNUnet ..." << endl;
                  service->async_setup(yield[ec]);

                  if (ec) {
                      cerr << "Failed to setup GNUnet service: "
                           << ec.message() << endl;
                      return;
                  }

                  cout << "GNUnet ID: " << service->identity() << endl;

                  client.gnunet_service = move(service);
              }
              else if (is_i2p_endpoint(injector_ep)) {
                  auto ep = boost::get<I2PEndpoint>(injector_ep).pubkey;

                  i2poui::Service service((REPO_ROOT/"i2p").native(), ios);
                  sys::error_code ec;
                  i2poui::Connector connector = service.build_connector(ep, yield[ec]);

                  if (ec) {
                      cerr << "Failed to setup I2Poui service: "
                           << ec.message() << endl;
                      return;
                  }

                  client.i2p =
                      make_unique<Client::I2P>(Client::I2P{move(service),
                              move(connector)});
              }

              do_listen( client
                       , local_ep
                       , ipns
                       , yield);
          });

    ios.run();

    return EXIT_SUCCESS;
}
