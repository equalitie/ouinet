#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <fstream>

#include <ipfs_cache/injector.h>
#include <gnunet_channels/channel.h>
#include <gnunet_channels/cadet_port.h>
#include <gnunet_channels/service.h>
#include <i2poui.h>

#include "namespaces.h"
#include "util.h"
#include "fetch_http_page.h"
#include "generic_connection.h"
#include "split_string.h"
#include "async_sleep.h"
#include "increase_open_file_limit.h"
#include "full_duplex_forward.h"

using namespace std;
using namespace ouinet;

namespace fs = boost::filesystem;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::dynamic_body>;

static fs::path REPO_ROOT;
static const fs::path OUINET_CONF_FILE = "ouinet-injector.conf";

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
static bool contains_private_data(const http::request_header<>& request)
{
    for (auto& field : request) {
        if(!util::field_is_one_of(field
                , http::field::host
                , http::field::user_agent
                , http::field::accept
                , http::field::accept_language
                , http::field::accept_encoding
                , http::field::keep_alive
                , http::field::connection
                , http::field::referer
                // https://www.w3.org/TR/upgrade-insecure-requests/
                , "Upgrade-Insecure-Requests"
                // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/DNT
                , "DNT")) {
            return true;
        }
    }

    // TODO: This may be a bit too agressive.
    if (request.method() != http::verb::get) {
        return true;
    }

    if (split_string_pair(request.target(), '?').second.size()) {
        return true;
    }

    return false;
}

//------------------------------------------------------------------------------
// Cache control:
// https://tools.ietf.org/html/rfc7234
// https://tools.ietf.org/html/rfc5861
// https://tools.ietf.org/html/rfc8246
//
// For a less dry reading:
// https://developers.google.com/web/fundamentals/performance/optimizing-content-efficiency/http-caching
//
// TODO: This function is incomplete.
static bool ok_to_cache( const http::request_header<>&  request
                       , const http::response_header<>& response)
{
    using boost::iequals;

    switch (response.result()) {
        case http::status::ok:
        case http::status::moved_permanently:
            break;
        // TODO: Other response codes
        default:
            return false;
    }

    auto req_cache_control_i = request.find(http::field::cache_control);

    if (req_cache_control_i != request.end()) {
        for (auto v : SplitString(req_cache_control_i->value(), ',')) {
            if (iequals(v, "no-store")) return false;
        }
    }

    auto cache_control_i = response.find(http::field::cache_control);

    // https://tools.ietf.org/html/rfc7234#section-3 (bullet #5)
    if (request.count(http::field::authorization)) {
        // https://tools.ietf.org/html/rfc7234#section-3.2
        if (cache_control_i == response.end()) return false;

        for (auto v : SplitString(cache_control_i->value(), ',')) {
            if (v == "must-revalidate") return true;
            if (v == "public")          return true;
            if (v == "s-maxage")        return true;
        }
    }

    if (cache_control_i == response.end()) return true;

    for (auto kv : SplitString(cache_control_i->value(), ','))
    {
        beast::string_view key, val;
        std::tie(key, val) = split_string_pair(kv, '=');

        // https://tools.ietf.org/html/rfc7234#section-3 (bullet #3)
        if (key == "no-store") return false;
        // https://tools.ietf.org/html/rfc7234#section-3 (bullet #4)
        if (key == "private")  {
            // NOTE: This decision based on the request having private data is
            // our extension (NOT part of RFC). Some servers (e.g.
            // www.bbc.com/) sometimes respond with 'Cache-Control: private'
            // even though the request doesn't contain any private data (e.g.
            // Cookies, {GET,POST,...} variables,...).  We believe this happens
            // when the server serves different content depending on the
            // client's geo location. While we don't necessarily want to break
            // this intent, we believe serving _some_ content is better than
            // none. As such, the client should always check for presence of
            // this 'private' field when fetching from distributed cache and
            // - if present - re-fetch from origin if possible.
            if (contains_private_data(request)) {
                return false;
            }
        }
    }

    return true;
}

//------------------------------------------------------------------------------
static
void try_to_cache( ipfs_cache::Injector& injector
                 , const http::request_header<>& request
                 , const http::response<http::dynamic_body>& response)
{
    bool do_cache = ok_to_cache(request, response);

    if (!do_cache) return;

    // TODO: This list was created by going through some 100 responses from
    // bbc.com. Careful selection from all possible (standard) fields is
    // needed.
    auto filtered_response =
        util::filter_fields( response
                           , http::field::server
                           , http::field::retry_after
                           , http::field::content_length
                           , http::field::content_type
                           , http::field::content_encoding
                           , http::field::content_language
                           , http::field::accept_ranges
                           , http::field::etag
                           , http::field::age
                           , http::field::date
                           , http::field::expires
                           , http::field::via
                           , http::field::vary
                           , http::field::connection
                           , http::field::location
                           , http::field::cache_control
                           , http::field::warning
                           , http::field::last_modified
                           // Not sure about these
                           , http::field::access_control_allow_origin
                           , http::field::access_control_allow_headers
                           , http::field::access_control_allow_methods
                           , http::field::access_control_allow_credentials
                           , http::field::access_control_max_age
                           );

    stringstream ss;
    ss << filtered_response;
    auto key = request.target().to_string();

    injector.insert_content(key, ss.str(),
        [key] (sys::error_code ec, auto) {
            if (ec) {
                cout << "!Insert failed: " << key << " " << ec.message() << endl;
            }
        });
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

    if (ec) {
        return handle_bad_request( client_c
                                 , req
                                 , "Connect: can't connect to the origin"
                                 , yield[ec]);
    }

    // Send the client an OK message indicating that the tunnel
    // has been established.
    http::response<http::empty_body> res{http::status::ok, req.version()};
    res.prepare_payload();

    http::async_write(client_c, res, yield[ec]);

    if (ec) return fail(ec, "sending connect response");

    full_duplex(client_c, origin_c, yield);
}

//------------------------------------------------------------------------------
static
void serve( GenericConnection con
          , ipfs_cache::Injector& injector
          , asio::yield_context yield)
{
    sys::error_code ec;
    beast::flat_buffer buffer;

    for (;;) {
        Request req;

        http::async_read(con, buffer, req, yield[ec]);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "read");

        if (req.method() == http::verb::connect) {
            return handle_connect_request(con, req, yield);
        }
        // Fetch the content from origin
        auto res = fetch_http_page(con.get_io_service(), req, ec, yield);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "fetch_http_page");

        try_to_cache(injector, req, res);

        // Forward back the response
        http::async_write(con, res, yield[ec]);
        if (ec) return fail(ec, "write");
    }
}

//------------------------------------------------------------------------------
template<class Connection>
static
void spawn_and_serve( Connection&& connection
                    , ipfs_cache::Injector& ipfs_cache_injector)
{
    auto& ios = connection.get_io_service();

    asio::spawn( ios
               , [ c = GenericConnection(move(connection))
                 , &ipfs_cache_injector
                 ](asio::yield_context yield) mutable {
                     serve(move(c), ipfs_cache_injector, yield);
                 });
}

//------------------------------------------------------------------------------
static
void listen_tcp( asio::io_service& ios
               , tcp::endpoint endpoint
               , ipfs_cache::Injector& ipfs_cache_injector
               , asio::yield_context yield)
{
    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(ios);

    acceptor.open(endpoint.protocol(), ec);
    if (ec) return fail(ec, "open");

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(endpoint, ec);
    if (ec) return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) return fail(ec, "listen");

    for(;;)
    {
        tcp::socket socket(ios);
        acceptor.async_accept(socket, yield[ec]);
        if(ec) {
            fail(ec, "accept");
            // Wait one second before we start accepting again.
            async_sleep(ios, chrono::seconds(1), yield);
        }
        else {
            spawn_and_serve(move(socket), ipfs_cache_injector);
        }
    }
}

//------------------------------------------------------------------------------
static
void listen_gnunet( asio::io_service& ios
                  , string port_str
                  , ipfs_cache::Injector& ipfs_cache_injector
                  , asio::yield_context yield)
{
    namespace gc = gnunet_channels;

    gc::Service service((REPO_ROOT/"gnunet"/"peer.conf").native(), ios);

    sys::error_code ec;

    cout << "Setting up GNUnet..." << endl;
    service.async_setup(yield[ec]);

    if (ec) {
        cerr << "Failed to setup GNUnet service: " << ec.message() << endl;
        return;
    }

    cout << "GNUnet ID: " << service.identity() << endl;

    gc::CadetPort port(service);

    while (true) {
        gc::Channel channel(service);
        port.open(channel, port_str, yield[ec]);

        if (ec) {
            cerr << "Failed to accept: " << ec.message() << endl;
            async_sleep(ios, chrono::milliseconds(100), yield);
            continue;
        }

        spawn_and_serve(move(channel), ipfs_cache_injector);
    }
}

//------------------------------------------------------------------------------
static
void listen_i2p( asio::io_service& ios
               , ipfs_cache::Injector& ipfs_cache_injector
               , asio::yield_context yield)
{
    i2poui::Service service((REPO_ROOT/"i2p").native(), ios);

    sys::error_code ec;

    cout << "Setting up I2Poui service..." << endl;
    i2poui::Acceptor acceptor = service.build_acceptor(yield[ec]);

    if (ec) {
        cerr << "Failed to setup I2Poui service: " << ec.message() << endl;
        return;
    }

    cout << "I2P Public ID: " << service.public_identity() << endl;


    while (true) {
        i2poui::Channel channel(service);
        acceptor.accept(channel, yield[ec]);

        if (ec) {
            cerr << "Failed to accept: " << ec.message() << endl;
            async_sleep(ios, chrono::milliseconds(100), yield);
            continue;
        }

        spawn_and_serve(move(channel), ipfs_cache_injector);
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
        ("listen-on-gnunet", po::value<string>(), "GNUnet port on which we'll listen")
        ("listen-on-i2p",
         po::value<string>(),
         "Whether we should be listening on I2P (true/false)")
        ("open-file-limit"
         , po::value<unsigned int>()
         , "To increase the maximum number of open files")
        ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        cout << desc << endl;
        return 0;
    }

    if (!vm.count("repo")) {
        cerr << "The 'repo' argument is missing" << endl;
        cerr << desc << endl;
        return 1;
    }

    REPO_ROOT = vm["repo"].as<string>();

    if (!exists(REPO_ROOT) || !is_directory(REPO_ROOT)) {
        cerr << "The path " << REPO_ROOT << " either doesn't exist or"
             << " is not a directory." << endl;
        cerr << desc << endl;
        return 1;
    }

    fs::path ouinet_conf_path = REPO_ROOT/OUINET_CONF_FILE;

    if (!fs::is_regular_file(ouinet_conf_path)) {
        cerr << "The path " << REPO_ROOT << " does not contain the "
             << OUINET_CONF_FILE << " configuration file." << endl;
        cerr << desc << endl;
        return 1;
    }

    ifstream ouinet_conf(ouinet_conf_path.native());

    po::store(po::parse_config_file(ouinet_conf, desc), vm);
    po::notify(vm);

    if (!vm.count("listen-on-tcp") && !vm.count("listen-on-gnunet") && !vm.count("listen-on-i2p")) {
        cerr << "One or more of {listen-on-tcp,listen-on-gnunet,listen-on-i2p}"
             << " must be provided." << endl;
        cerr << desc << endl;
        return 1;
    }

    if (vm.count("open-file-limit")) {
        increase_open_file_limit(vm["open-file-limit"].as<unsigned int>());
    }

    bool listen_on_i2p = false;

    // Unfortunately, Boost.ProgramOptions doesn't support arguments without
    // values in config files. Thus we need to force the 'listen-on-i2p' arg
    // to have one of the strings values "true" or "false".
    if (vm.count("listen-on-i2p")) {
        auto value = vm["listen-on-i2p"].as<string>();

        if (value != "" && value != "true" && value != "false") {
            cerr << "The listen-on-i2p parameter may be either 'true' or 'false'"
                 << endl;
            return 1;
        }

        listen_on_i2p = value == "true";
    }

    // The io_service is required for all I/O
    asio::io_service ios;

    ipfs_cache::Injector ipfs_cache_injector(ios, (REPO_ROOT/"ipfs").native());

    std::cout << "IPNS DB: " << ipfs_cache_injector.ipns_id() << endl;

    if (vm.count("listen-on-tcp")) {
        auto const injector_ep
            = util::parse_endpoint(vm["listen-on-tcp"].as<string>());

        asio::spawn
            ( ios
            , [&, injector_ep](asio::yield_context yield) {
                  listen_tcp(ios, injector_ep, ipfs_cache_injector, yield);
              });
    }

    if (vm.count("listen-on-gnunet")) {
        asio::spawn
            ( ios
            , [&](asio::yield_context yield) {
                  string port = vm["listen-on-gnunet"].as<string>();
                  listen_gnunet(ios, port, ipfs_cache_injector, yield);
              });
    }

    if (listen_on_i2p) {
        asio::spawn
            ( ios
            , [&](asio::yield_context yield) {
                  listen_i2p(ios, ipfs_cache_injector, yield);
              });
    }

    ios.run();

    return EXIT_SUCCESS;
}
