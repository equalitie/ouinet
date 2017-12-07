#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/connect.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <fstream>

#include <ipfs_cache/client.h>
#include <ipfs_cache/error.h>
#include <ipfs_cache/timer.h>

#include <gnunet_channels/channel.h>
#include <gnunet_channels/service.h>

#include "namespaces.h"
#include "connect_to_host.h"
#include "fetch_http_page.h"
#include "client_front_end.h"
#include "generic_connection.h"
#include "util.h"
#include "result.h"
#include "blocker.h"
#include "request_routing.h"

using namespace std;
using namespace ouinet;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::string_body>;

static string REPO_ROOT;

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

    http::response<http::empty_body> res{http::status::ok, req.version()};

    // Send the client an OK message indicating that the tunnel
    // has been established. TODO: Reply with an error otherwise.
    http::async_write(client_c, res, yield[ec]);
    if (ec) return fail(ec, "sending connect response");

    Blocker blocker(ios);

    asio::spawn
        ( yield
        , [&, b = blocker.make_block()](asio::yield_context yield) {
              forward(client_c, *origin_c, yield);
          });

    asio::spawn
        ( yield
        , [&, b = blocker.make_block()](asio::yield_context yield) {
              forward(*origin_c, client_c, yield);
          });

    blocker.wait(yield);
}

//------------------------------------------------------------------------------
static
Result<unique_ptr<GenericConnection>>
connect_to_injector( string endpoint
                   , gnunet_channels::Service& service
                   , asio::yield_context yield)
{
    auto as_tcp_endpoint = []( string_view host
                             , string_view port
                             ) -> Result<tcp::endpoint> {
        sys::error_code ec;
        auto ip = asio::ip::address::from_string(host.to_string(), ec);
        if (ec) return ec;
        return tcp::endpoint(ip, strtol(port.data(), 0, 10));
    };

    sys::error_code ec;

    string_view host;
    string_view port;

    std::tie(host, port) = util::split_host_port(endpoint);

    // TODO: Currently this code only recognizes as host an IP address (v4 or
    // v6) and GNUnet's hash. Would be nice to support standard web hosts of
    // type "foobar.com" as well.
    if (auto ep = as_tcp_endpoint(host, port)) {
        tcp::socket socket(service.get_io_service());
        socket.async_connect(*ep, yield[ec]);

        if (ec) return ec;
        return make_unique<GenericConnectionImpl<tcp::socket>>(move(socket));
    }
    else {
        // Interpret as gnunet endpoint
        using Channel = gnunet_channels::Channel;

        Channel ch(service);
        ch.connect(host.to_string(), port.to_string(), yield[ec]);

        if (ec) return ec;
        return make_unique<GenericConnectionImpl<Channel>>(move(ch));
    }
}

//------------------------------------------------------------------------------
static void serve_request( shared_ptr<GenericConnection> con
                         , string injector
                         , shared_ptr<ipfs_cache::Client> cache_client
                         , shared_ptr<ClientFrontEnd> front_end
                         , gnunet_channels::Service& gnunet_service
                         , asio::yield_context yield)
{
    sys::error_code ec;
    beast::flat_buffer buffer;
    // These hard-wired access mechanisms are attempted in order for all normal requests.
    const vector<enum request_mechanism> req_mechs({request_mechanism::cache, request_mechanism::injector});
    // These are only attempted if their targets match the regular expressions:
    //const vector<enum request_mechanism> match_rmechs({request_mechanism::cache});
    // Regular expressions for matching request targets:
    //const vector<boost::regex> target_rxs({boost::regex("https?://(www\\.)?example.com/.*")});
    // Matches/mechanisms to test the request against.
    using Match = pair<const RequestMatch&, const vector<enum request_mechanism>&>;
    const vector<Match> matches({
        Match( RegexRequestMatch([](const Request& r) {return r["Host"];}, boost::regex("https?://(www\\.)?example.com/.*"))
             , {request_mechanism::cache}),
        Match( RegexRequestMatch([](const Request& r) {return r["Host"];}, boost::regex("https?://(www\\.)?example.net/.*"))
             , {request_mechanism::cache, request_mechanism::injector, request_mechanism::origin}),
    });

    // Process the different requests that may come over the same connection.
    for (;;) {  // continue for next request; break for no more requests
        Request req;

        // Read the (clear-text) HTTP request
        http::async_read(*con, buffer, req, yield[ec]);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "read");

        // Attempt connection to origin for CONNECT requests
        if (req.method() == http::verb::connect) {
            return handle_connect_request(*con, req, yield);
        }

        // At this point we have access to the plain text HTTP proxy request.
        // Attempt the different mechanisms provided by the routing component.

        // NOTE: We need to use the 'std::' prefix here due to ADL
        //       (http://en.cppreference.com/w/cpp/language/adl)

        // This uses the same list of mechanisms for all requests.
        //unique_ptr<RequestRouter> router = std::make_unique<SimpleRequestRouter>(req, req_mechs);
        // This uses one list of mechanisms for requests matching one of a list or regular expressions,
        // or a default list for the ones that do not.
        //unique_ptr<RequestRouter> router = std::make_unique<MatchTargetRequestRouter>(req, target_rxs, match_rmechs, req_mechs);
        // This uses a different list of mechanisms for each regular expression that the request may match,
        // or a default list for the ones that do not.
        unique_ptr<RequestRouter> router = std::make_unique<MultiMatchRequestRouter>(req, matches, req_mechs);

        for (;;) {  // continue for next mechanism; break for next request
            auto req_mech = router->get_next_mechanism(ec);
            if (ec) {
                return handle_bad_request(*con, req, ec.message(), yield);
            }
            // cout << "Attempt " << req.method_string() << ' ' << req.target()
            //      << " via mechanism #" << req_mech << endl;

            // Serve requests targeted to the client front end
            if (req_mech == request_mechanism::_front_end) {
                return front_end->serve(*con, req, cache_client, yield);
            }

            // Get the content from cache (if available and enabled)
            if (req_mech == request_mechanism::cache) {
                if (!cache_client || !front_end->is_ipfs_cache_enabled()) {
                    continue;  // next mechanism
                }

                auto key = req.target();  // use request absolute URI as key

                string content = cache_client->get_content(key.to_string(), yield[ec]);

                if (ec) {
                    if (ec != ipfs_cache::error::key_not_found) {
                        cout << "Failed to fetch from DB " << ec.message()
                             << " " << req.target() << endl;
                    }
                    continue;  // next mechanism
                }

                asio::async_write(*con, asio::buffer(content), yield[ec]);
                if (ec) return fail(ec, "async_write");
                break;  // next request
            }

            // Get the content from injector (if enabled)
            if (req_mech == request_mechanism::injector) {
                if (!front_end->is_injector_proxying_enabled()) {
                    continue;  // next mechanism
                }

                auto inj_con = connect_to_injector(injector, gnunet_service, yield);

                if (inj_con.is_error()) return fail(inj_con.get_error(), "channel connect");

                // Forward the request to the injector
                auto res = fetch_http_page(con->get_io_service(), **inj_con, req, ec, yield);
                if (ec) return fail(ec, "fetch_http_page");

                // Forward back the response
                http::async_write(*con, res, yield[ec]);
                if (ec && ec != http::error::end_of_stream) return fail(ec, "write");
                break;  // next request
            }

            // Requests going to the origin are not supported
            // (this includes non-safe HTTP requests like POST).
            // TODO: We're not handling HEAD requests correctly.
            return handle_bad_request(*con, req, "Unsupported request mechanism", yield);
        }
    }
}

//------------------------------------------------------------------------------
static void async_sleep( asio::io_service& ios
                       , asio::steady_timer::duration duration
                       , asio::yield_context yield)
{
    asio::steady_timer timer(ios);
    timer.expires_from_now(duration);
    sys::error_code ec;
    timer.async_wait(yield[ec]);
}

//------------------------------------------------------------------------------
void do_listen( asio::io_service& ios
              , gnunet_channels::Service& gnunet_service
              , tcp::endpoint local_endpoint
              , string injector
              , string ipns
              , asio::yield_context yield)
{
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

    shared_ptr<ipfs_cache::Client> ipfs_cache_client;

    if (ipns.size()) {
        ipfs_cache_client = make_shared<ipfs_cache::Client>(ios, ipns, REPO_ROOT + "/ipfs");
    }

    cout << "Client accepting on " << acceptor.local_endpoint() << endl;

    auto front_end = make_shared<ClientFrontEnd>();

    for(;;)
    {
        tcp::socket socket(ios);
        acceptor.async_accept(socket, yield[ec]);
        if(ec) {
            fail(ec, "accept");
            async_sleep(ios, chrono::seconds(1), yield);
        }
        else {
            asio::spawn( ios
                       , [ s = move(socket)
                         , ipfs_cache_client
                         , injector
                         , front_end
                         , &gnunet_service
                         ](asio::yield_context yield) mutable {
                             using Con = GenericConnectionImpl<tcp::socket>;
                             auto con = make_shared<Con>(move(s));

                             serve_request( con
                                          , move(injector)
                                          , move(ipfs_cache_client)
                                          , move(front_end)
                                          , gnunet_service
                                          , yield);

                             sys::error_code ec;
                             con->get_impl().shutdown(tcp::socket::shutdown_send, ec);
                         });
        }
    }
}

//------------------------------------------------------------------------------
#include <sys/resource.h>
// Temporary, until this is merged https://github.com/ipfs/go-ipfs/pull/4288
// into IPFS.
void bump_file_limit(rlim_t new_value)
{
   struct rlimit rl;

   int r = getrlimit(RLIMIT_NOFILE, &rl);

   if (r != 0) {
       cerr << "Failed to get the current RLIMIT_NOFILE value" << endl;
       return;
   }

   cout << "Default RLIMIT_NOFILE value is: " << rl.rlim_cur << endl;

   if (rl.rlim_cur >= new_value) {
       cout << "Leaving RLIMIT_NOFILE value unchanged." << endl;
       return;
   }

   rl.rlim_cur = new_value;

   r = setrlimit(RLIMIT_NOFILE, &rl);

   if (r != 0) {
       cerr << "Failed to set the RLIMIT_NOFILE value to " << rl.rlim_cur << endl;
       return;
   }

   r = getrlimit(RLIMIT_NOFILE, &rl);
   assert(r == 0);
   cout << "RLIMIT_NOFILE value changed to: " << rl.rlim_cur << endl;
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
        ("open-file-limit"
         , po::value<unsigned int>()
         , "To increase the number of open files")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (!vm.count("repo")) {
        cerr << "The 'repo' argument is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    REPO_ROOT = vm["repo"].as<string>();

    ifstream ouinet_conf(REPO_ROOT + "/ouinet-client.conf");

    po::store(po::parse_config_file(ouinet_conf, desc), vm);
    po::notify(vm);

    if (vm.count("open-file-limit")) {
        bump_file_limit(vm["open-file-limit"].as<unsigned int>());
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
    auto const injector = vm["injector-ep"].as<string>();

    string ipns;

    if (vm.count("injector-ipns")) {
        ipns = vm["injector-ipns"].as<string>();
    }

    // The io_service is required for all I/O
    asio::io_service ios;

    asio::spawn
        ( ios
        , [&](asio::yield_context yield) {
              namespace gc = gnunet_channels;

              gc::Service service(REPO_ROOT + "/gnunet/peer.conf", ios);

              sys::error_code ec;

              service.async_setup(yield[ec]);

              if (ec) {
                  cerr << "Failed to setup GNUnet service: " << ec.message() << endl;
                  return;
              }

              cout << "GNUnet ID: " << service.identity() << endl;

              do_listen( ios
                       , service
                       , local_ep
                       , injector
                       , ipns
                       , yield);
          });

    ios.run();

    return EXIT_SUCCESS;
}
