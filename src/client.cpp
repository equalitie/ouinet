#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <iostream>
#include <fstream>
#include <cstdlib>  // for atexit()

#include <ipfs_cache/client.h>
#include <ipfs_cache/error.h>

#include "namespaces.h"
#include "fetch_http_page.h"
#include "client_front_end.h"
#include "generic_connection.h"
#include "util.h"
#include "async_sleep.h"
#include "increase_open_file_limit.h"
#include "endpoint.h"
#include "cache_control.h"
#include "or_throw.h"
#include "request_routing.h"
#include "full_duplex_forward.h"
#include "client_config.h"
#include "client.h"
#include "authenticate.h"

#ifdef __ANDROID__
#  include "redirect_to_android_log.h"
#else // ifdef __ANDROID__
#  include "force_exit_on_signal.h"
#endif // else ifdef __ANDROID__

#include "ouiservice.h"
#include "ouiservice/i2p.h"
#include "ouiservice/tcp.h"

#include "util/signal.h"

using namespace std;
using namespace ouinet;

namespace posix_time = boost::posix_time;

using tcp         = asio::ip::tcp;
using Request     = http::request<http::string_body>;
using Response    = http::response<http::dynamic_body>;
using boost::optional;

static const boost::filesystem::path OUINET_PID_FILE = "pid";

//------------------------------------------------------------------------------
#define ASYNC_DEBUG(code, ...) [&] () mutable {\
    auto task = _front_end.notify_task(util::str(__VA_ARGS__));\
    return code;\
}()

//------------------------------------------------------------------------------
class Client::State : public enable_shared_from_this<Client::State> {
    friend class Client;

public:
    State(asio::io_service& ios)
        : _ios(ios)
#ifdef __ANDROID__
        , _cout_guard(std::cout)
        , _cerr_guard(std::cerr)
#endif // ifdef __ANDROID__
    {}

    void start(int argc, char* argv[]);
    void stop() { _shutdown_signal(); }

    void setup_ipfs_cache(const string& ipns);
    void set_injector(string);

private:
    void serve_request(GenericConnection& con, asio::yield_context yield);

    void handle_connect_request( GenericConnection& client_c
                               , const Request& req
                               , asio::yield_context yield);

    CacheControl::CacheEntry
    fetch_stored( const Request& request
                , request_route::Config& request_config
                , asio::yield_context yield);

    Response fetch_fresh( const Request& request
                        , request_route::Config& request_config
                        , asio::yield_context yield);

    CacheControl build_cache_control(request_route::Config& request_config);

    void listen_tcp( asio::yield_context
                   , tcp::endpoint
                   , function<void(GenericConnection, asio::yield_context)>);

    void setup_injector(asio::yield_context);

    boost::filesystem::path get_pid_path() const {
        return _config.repo_root()/OUINET_PID_FILE;
    }


private:
    asio::io_service& _ios;
    ClientConfig _config;
    std::unique_ptr<OuiServiceClient> _injector;
    std::unique_ptr<ipfs_cache::Client> _ipfs_cache;

    ClientFrontEnd _front_end;
    Signal<void()> _shutdown_signal;

#ifdef __ANDROID__
    // While these two objects are 'alive', everything that is written
    // into std::{cout,cerr} will be written into Android's log.
    RedirectToAndroidLog _cout_guard;
    RedirectToAndroidLog _cerr_guard;
#endif // ifdef __ANDROID__

    unique_ptr<util::PidFile> _pid_file;
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
void Client::State::handle_connect_request( GenericConnection& client_c
                                          , const Request& req
                                          , asio::yield_context yield)
{
    // https://tools.ietf.org/html/rfc2817#section-5.2

    sys::error_code ec;

    if (!_front_end.is_injector_proxying_enabled()) {
        return ASYNC_DEBUG( handle_bad_request( client_c
                                              , req
                                              , "Forwarding disabled"
                                              , yield[ec])
                          , "Forwarding disabled");
    }

    auto inj = _injector->connect(yield[ec], _shutdown_signal);

    if (ec) {
        // TODO: Does an RFC dicate a particular HTTP status code?
        return handle_bad_request(client_c, req, "Can't connect to injector", yield[ec]);
    }

    auto disconnect_injector_slot = _shutdown_signal.connect([&inj] {
        inj.connection.close();
    });

    auto credentials = _config.credentials_for(inj.remote_endpoint);

    if (credentials) {
        auto auth_req = authorize(req, *credentials);
        http::async_write(inj.connection, auth_req, yield[ec]);
    }
    else {
        http::async_write(inj.connection, const_cast<Request&>(req), yield[ec]);
    }

    if (ec) {
        // TODO: Does an RFC dicate a particular HTTP status code?
        return handle_bad_request(client_c, req, "Can't contact the injector", yield[ec]);
    }

    beast::flat_buffer buffer;
    Response res;
    http::async_read(inj.connection, buffer, res, yield[ec]);

    if (ec) {
        // TODO: Does an RFC dicate a particular HTTP status code?
        return handle_bad_request(client_c, req, "Can't contact the injector", yield[ec]);
    }

    http::async_write(client_c, res, yield[ec]);

    if (ec) return fail(ec, "sending connect response");

    if (!(200 <= unsigned(res.result()) && unsigned(res.result()) < 300)) {
        return;
    }

    full_duplex(client_c, inj.connection, yield);
}

//------------------------------------------------------------------------------
CacheControl::CacheEntry
Client::State::fetch_stored( const Request& request
                           , request_route::Config& request_config
                           , asio::yield_context yield)
{
    using CacheEntry = CacheControl::CacheEntry;

    const bool cache_is_disabled
        = !request_config.enable_cache
       || !_ipfs_cache
       || !_front_end.is_ipfs_cache_enabled();

    if (cache_is_disabled) {
        return or_throw<CacheControl::CacheEntry>( yield ,
                asio::error::operation_not_supported);
    }

    sys::error_code ec;
    // Get the content from cache
    auto key = request.target();

    auto content = _ipfs_cache->get_content(key.to_string(), yield[ec]);

    if (ec) return or_throw<CacheEntry>(yield, ec);

    // If the content does not have a meaningful time stamp,
    // an error should have been reported.
    assert(!content.ts.is_not_a_date_time());

    http::response_parser<Response::body_type> parser;
    parser.eager(true);
    parser.put(asio::buffer(content.data), ec);

    assert(!ec && "Malformed cache entry");

    if (!parser.is_done()) {
#ifndef NDEBUG
        cerr << "------- WARNING: Unfinished message in cache --------" << endl;
        assert(parser.is_header_done() && "Malformed response head did not cause error");
        auto response = parser.get();
        cerr << request << response.base() << "<" << response.body().size() << " bytes in body>" << endl;
        cerr << "-----------------------------------------------------" << endl;
#endif
        ec = asio::error::not_found;
    }

    if (ec) return or_throw<CacheEntry>(yield, ec);

    return CacheEntry{content.ts, parser.release()};
}

//------------------------------------------------------------------------------
Response Client::State::fetch_fresh( const Request& request
                                   , request_route::Config& request_config
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
                if (!_front_end.is_injector_proxying_enabled()) {
                    continue;
                }
                sys::error_code ec;

                auto inj
                    = _injector->connect(yield[ec], _shutdown_signal);

                if (ec) {
                    last_error = ec;
                    continue;
                }

                auto credentials = _config.credentials_for(inj.remote_endpoint);

                Response res;

                // Forward the request to the injector
                if (credentials) {
                    res = fetch_http_page(_ios
                                         , inj.connection
                                         , authorize(request, *credentials)
                                         , yield[ec]);
                }
                else {
                    res = fetch_http_page(_ios
                                         , inj.connection
                                         , request
                                         , yield[ec]);
                }

                if (!ec) return res;
                last_error = ec;
                continue;
            }
            case responder::_front_end: {
                return _front_end.serve( _config.injector_endpoint()
                                       , request
                                       , _ipfs_cache.get());
            }
        }
    }

    return or_throw<Response>(yield, last_error);
}

//------------------------------------------------------------------------------
CacheControl
Client::State::build_cache_control(request_route::Config& request_config)
{
    CacheControl cache_control;

    cache_control.fetch_stored =
        [&] (const Request& request, asio::yield_context yield) {
            return ASYNC_DEBUG( fetch_stored(request, request_config, yield)
                              , "Fetch from cache: " , request.target());
        };

    cache_control.fetch_fresh =
        [&] (const Request& request, asio::yield_context yield) {
            return ASYNC_DEBUG( fetch_fresh(request, request_config, yield)
                              , "Fetch from origin: ", request.target());
        };

    cache_control.max_cached_age(_config.max_cached_age());

    return cache_control;
}

//------------------------------------------------------------------------------
static
Response bad_gateway(const Request& req)
{
    Response res{http::status::bad_gateway, req.version()};
    res.set(http::field::server, "Ouinet");
    res.keep_alive(req.keep_alive());
    return res;
}

//------------------------------------------------------------------------------
void Client::State::serve_request( GenericConnection& con
                                 , asio::yield_context yield)
{
    namespace rr = request_route;
    using rr::responder;

    auto close_con_slot = _shutdown_signal.connect([&con] {
        con.close();
    });

    // These access mechanisms are attempted in order for requests by default.
    const rr::Config default_request_config
        { true
        , queue<responder>({responder::injector})};

    rr::Config request_config;

    CacheControl cache_control = build_cache_control(request_config);

    sys::error_code ec;
    beast::flat_buffer buffer;

    // Expressions to test the request against and mechanisms to be used.
    // TODO: Create once and reuse.
    using Match = pair<const ouinet::reqexpr::reqex, const rr::Config>;

    auto method_getter([](const Request& r) {return r.method_string();});
    auto host_getter([](const Request& r) {return r["Host"];});
    auto x_oui_dest_getter([](const Request& r) {return r["X-Oui-Destination"];});
    auto target_getter([](const Request& r) {return r.target();});

    const vector<Match> matches({
        // Handle requests to <http://localhost/> internally.
        Match( reqexpr::from_regex(host_getter, "localhost")
             , {false, queue<responder>({responder::_front_end})} ),

        Match( reqexpr::from_regex(x_oui_dest_getter, "OuiClient")
             , {false, queue<responder>({responder::_front_end})} ),

        // NOTE: The matching of HTTP methods below can be simplified,
        // leaving expanded for readability.

        // NOTE: The injector mechanism is temporarily used in some matches
        // instead of the mechanisms following it (commented out)
        // since the later are not implemented yet.

        // Send unsafe HTTP method requests to the origin server
        // (or the proxy if that does not work).
        // NOTE: The cache need not be disabled as it should know not to
        // fetch requests in these cases.
        Match( !reqexpr::from_regex(method_getter, "(GET|HEAD|OPTIONS|TRACE)")
             , {false, queue<responder>({responder::injector/*responder::origin, responder::proxy*/})} ),
        // Do not use cache for safe but uncacheable HTTP method requests.
        // NOTE: same as above.
        Match( reqexpr::from_regex(method_getter, "(OPTIONS|TRACE)")
             , {false, queue<responder>({responder::injector/*responder::origin, responder::proxy*/})} ),
        // Do not use cache for validation HEADs.
        // Caching these is not yet supported.
        Match( reqexpr::from_regex(method_getter, "HEAD")
             , {false, queue<responder>({responder::injector})} ),
        // Force cache and default mechanisms for this site.
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example.com/.*")
             , {true, queue<responder>()} ),
        // Force cache and particular mechanisms for this site.
        Match( reqexpr::from_regex(target_getter, "https?://(www\\.)?example.net/.*")
             , {true, queue<responder>({responder::injector})} ),
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
            if (_config.enable_http_connect_requests()) {
                auto res = bad_gateway(req);
                http::async_write(con, res, yield[ec]);
            }
            else {
                ASYNC_DEBUG( handle_connect_request(con, req, yield)
                           , "Connect ", req.target());
            }

            return;
        }

        request_config = route_choose_config(req, matches, default_request_config);

        auto res = ASYNC_DEBUG( cache_control.fetch(req, yield[ec])
                              , "Fetch "
                              , req.target());

        if (ec) {
#ifndef NDEBUG
            cerr << "----- WARNING: Error fetching --------" << endl;
            cerr << "Error Code: " << ec.message() << endl;
            cerr << req.base() << res.base() << endl;
            cerr << "--------------------------------------" << endl;
#endif

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
void Client::State::setup_ipfs_cache(const string& ipns)
{
    if (_ipfs_cache) {
        return _ipfs_cache->set_ipns(move(ipns));
    }

    try {
        ipfs_cache::Client cache( _ios
                                , ipns
                                , (_config.repo_root()/"ipfs").native());

        _ipfs_cache = make_unique<ipfs_cache::Client>(move(cache));
    }
    catch(std::exception& e) {
        cerr << "Client::State::set_ipns exception: " << e.what() << endl;
    }
}

//------------------------------------------------------------------------------
void Client::State::listen_tcp
        ( asio::yield_context yield
        , tcp::endpoint local_endpoint
        , function<void(GenericConnection, asio::yield_context)> handler)
{
    const auto ipns = _config.ipns();

    sys::error_code ec;

    // Open the acceptor
    tcp::acceptor acceptor(_ios);

    acceptor.open(local_endpoint.protocol(), ec);
    if (ec) return fail(ec, "open");

    acceptor.set_option(asio::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor.bind(local_endpoint, ec);
    if (ec) return fail(ec, "bind");

    // Start listening for connections
    acceptor.listen(asio::socket_base::max_connections, ec);
    if (ec) return fail(ec, "listen");

    auto shutdown_acceptor_slot = _shutdown_signal.connect([&acceptor] {
        acceptor.close();
    });

    if (ipns.size()) {
        setup_ipfs_cache(ipns);
    }

    auto shutdown_ipfs_slot = _shutdown_signal.connect([this] {
        _ipfs_cache = nullptr;
    });

    cout << "Client accepting on " << acceptor.local_endpoint() << endl;

    WaitCondition wait_condition(_ios);

    for(;;)
    {
        tcp::socket socket(_ios);
        acceptor.async_accept(socket, yield[ec]);

        if(ec) {
            if (ec == asio::error::operation_aborted) break;
            fail(ec, "accept");
            if (!async_sleep(_ios, chrono::seconds(1), _shutdown_signal, yield)) {
                break;
            }
        } else {
            static const auto tcp_shutter = [](tcp::socket& s) {
                sys::error_code ec; // Don't throw
                s.shutdown(tcp::socket::shutdown_both, ec);
                s.close(ec);
            };

            GenericConnection connection(move(socket) , move(tcp_shutter));

            asio::spawn( _ios
                       , [ this
                         , c = move(connection)
                         , h = move(handler)
                         , lock = wait_condition.lock()
                         ](asio::yield_context yield) mutable {
                             h(move(c), yield);
                         });
        }
    }

    wait_condition.wait(yield);
}

//------------------------------------------------------------------------------
void Client::State::start(int argc, char* argv[])
{
    _config = ClientConfig(argc, argv);

#ifndef __ANDROID__
    auto pid_path = get_pid_path();
    if (exists(pid_path)) {
        throw runtime_error(util::str
             ( "Existing PID file ", pid_path
             , "; another client process may be running"
             , ", otherwise please remove the file."));
    }
    // Acquire a PID file for the life of the process
    assert(!_pid_file);
    _pid_file = make_unique<util::PidFile>(pid_path);
#endif

    asio::spawn
        ( _ios
        , [this, self = shared_from_this()]
          (asio::yield_context yield) {
              sys::error_code ec;

              setup_injector(yield[ec]);

              if (ec) {
                  cerr << "Failed to setup injector" << endl;
              }

              listen_tcp( yield[ec]
                        , _config.local_endpoint()
                        , [this, self]
                          (GenericConnection c, asio::yield_context yield) {
                      serve_request(c, yield);
                  });
          });

    if (_config.front_end_endpoint() != tcp::endpoint()) {
        asio::spawn
            ( _ios
            , [this, self = shared_from_this()]
              (asio::yield_context yield) {
                  sys::error_code ec;

                  auto ep = _config.front_end_endpoint();
                  if (ep == tcp::endpoint()) return;

                  listen_tcp( yield[ec]
                            , ep
                            , [this, self]
                              (GenericConnection c, asio::yield_context yield) {
                        sys::error_code ec;
                        Request rq;
                        beast::flat_buffer buffer;
                        http::async_read(c, buffer, rq, yield[ec]);

                        if (ec) return;

                        auto rs = _front_end.serve( _config.injector_endpoint()
                                                  , rq
                                                  , _ipfs_cache.get());

                        http::async_write(c, rs, yield[ec]);
                  });
              });
    }
}

//------------------------------------------------------------------------------
void Client::State::setup_injector(asio::yield_context yield)
{
    _injector = std::make_unique<OuiServiceClient>(_ios);

    auto injector_ep = _config.injector_endpoint();

    if (!injector_ep) return;

    if (is_i2p_endpoint(*injector_ep)) {
        std::string ep = boost::get<I2PEndpoint>(*injector_ep).pubkey;
        auto i2p_service = make_shared<ouiservice::I2pOuiService>((_config.repo_root()/"i2p").string(), _ios);
        std::unique_ptr<ouiservice::I2pOuiServiceClient> i2p_client = i2p_service->build_client(ep);

        _injector->add(std::move(i2p_client));
    } else {
        tcp::endpoint tcp_endpoint
            = boost::get<asio::ip::tcp::endpoint>(*injector_ep);

        auto tcp_client
            = make_unique<ouiservice::TcpOuiServiceClient>(_ios, tcp_endpoint);

        _injector->add(std::move(tcp_client));
    }

    _injector->start(yield);
}

//------------------------------------------------------------------------------
void Client::State::set_injector(string injector_ep_str)
{
    // XXX: Workaround.
    // Eventually, OuiServiceClient should just support multiple parallel
    // active injector EPs.

    auto injector_ep = parse_endpoint(injector_ep_str);

    if (!injector_ep) {
        cerr << "Failed to parse endpoint \"" << injector_ep_str << "\"" << endl;
        return;
    }

    auto current_ep = _config.injector_endpoint();

    if (current_ep && *injector_ep == *current_ep) {
        return;
    }

    _config.set_injector_endpoint(*injector_ep);

    asio::spawn(_ios, [self = shared_from_this()] (auto yield) {
            sys::error_code ec;
            self->setup_injector(yield[ec]);
        });
}

//------------------------------------------------------------------------------
Client::Client(asio::io_service& ios)
    : _state(make_shared<State>(ios))
{}

Client::~Client()
{
}

void Client::start(int argc, char* argv[])
{
    _state->start(argc, argv);
}

void Client::stop()
{
    _state->stop();
}

void Client::set_injector_endpoint(const char* injector_ep)
{
    _state->set_injector(injector_ep);
}

void Client::set_ipns(const char* ipns)
{
    _state->_config.set_ipns(move(ipns));
}

void Client::set_credentials(const char* injector, const char* cred)
{
    _state->_config.set_credentials(injector, cred);
}

boost::filesystem::path Client::get_pid_path() const
{
    return _state->get_pid_path();
}

//------------------------------------------------------------------------------
#ifndef __ANDROID__
int main(int argc, char* argv[])
{
    asio::io_service ios;

    asio::signal_set signals(ios, SIGINT, SIGTERM);

    Client client(ios);

    unique_ptr<ForceExitOnSignal> force_exit;

    signals.async_wait([&client, &signals, &ios, &force_exit]
                       (const sys::error_code& ec, int signal_number) {
            client.stop();
            signals.clear();
            force_exit = make_unique<ForceExitOnSignal>();
        });

    try {
        client.start(argc, argv);

        static auto pid_file_path = client.get_pid_path();
        // Force removal of PID file on abnormal exit
        std::atexit([] {
                if (!exists(pid_file_path)) return;
                cerr << "Warning: not a clean exit" << endl;
                remove(pid_file_path);
            });
    } catch (std::exception& e) {
        cerr << e.what() << endl;
        return 1;
    }

    ios.run();

    return EXIT_SUCCESS;
}
#endif
