#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <iostream>

#include <ipfs_cache/injector.h>

#include "namespaces.h"
#include "fetch_http_page.h"
#include "generic_connection.h"

using namespace std;
using namespace ouinet;

using tcp         = asio::ip::tcp;
using string_view = beast::string_view;
using Request     = http::request<http::dynamic_body>;

//------------------------------------------------------------------------------
template<class Fields>
static bool ok_to_cache(const http::response_header<Fields>& hdr)
{
    using string_view = beast::string_view;

    auto cc_i = hdr.find(http::field::cache_control);

    if (cc_i == hdr.end()) return true;

    auto trim_whitespace = [](string_view& v) {
        while (v.starts_with(' ')) v.remove_prefix(1);
        while (v.ends_with  (' ')) v.remove_suffix(1);
    };

    auto key_val = [&trim_whitespace](string_view v) {
        auto eq = v.find('=');

        if (eq == string_view::npos) {
            trim_whitespace(v);
            return make_pair(v, string_view("", 0));
        }

        auto key = v.substr(0, eq);
        auto val = v.substr(eq + 1, v.size());

        trim_whitespace(key);
        trim_whitespace(val);

        return make_pair(key, val);
    };

    auto for_each = [&key_val] (string_view v, auto can_cache) {
        while (true) {
            auto comma = v.find(',');

            if (comma == string_view::npos) {
                if (v.size()) {
                    if (!can_cache(key_val(v))) return false;
                }
                break;
            }

            if (!can_cache(key_val(v.substr(0, comma)))) return false;
            v.remove_prefix(comma + 1);
        }

        return true;
    };

    return for_each(cc_i->value(), [] (auto kv) {
        auto key = kv.first;
        //auto val = kv.second;
        // https://developers.google.com/web/fundamentals/performance/optimizing-content-efficiency/http-caching
        if (key == "no-store")              return false;
        //if (key == "no-cache")              return false;
        //if (key == "max-age" && val == "0") return false;
        return true;
    });
}

//------------------------------------------------------------------------------
static
void serve( shared_ptr<GenericConnection> con
          , shared_ptr<ipfs_cache::Injector> injector
          , asio::yield_context yield)
{
    sys::error_code ec;
    beast::flat_buffer buffer;

    for (;;) {
        Request req;

        http::async_read(*con, buffer, req, yield[ec]);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "read");

        // Fetch the content from origin
        auto res = fetch_http_page(con->get_io_service(), req, ec, yield);

        if (ec == http::error::end_of_stream) break;
        if (ec) return fail(ec, "fetch_http_page");

        if (ok_to_cache(res)) {
            stringstream ss;
            ss << res;
            auto key = req.target().to_string();

            injector->insert_content(key , ss.str(), [key] (sys::error_code ec, auto) {
                    if (ec) {
                        cout << "!Insert failed: " << key << " " << ec.message() << endl;
                    }
                });
        }

        // Forward back the response
        http::async_write(*con, res, yield[ec]);
        if (ec) return fail(ec, "write");
    }
}

//------------------------------------------------------------------------------
void start( asio::io_service& ios
          , tcp::endpoint endpoint
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

    auto ipfs_cache_injector = make_shared<ipfs_cache::Injector>(ios, "injector_repo");

    std::cout << "IPNS DB: " << ipfs_cache_injector->ipns_id() << endl;

    for(;;)
    {
        tcp::socket socket(ios);
        acceptor.async_accept(socket, yield[ec]);
        if(ec) {
            fail(ec, "accept");
            // Wait one second before we start accepting again.
            asio::steady_timer timer(ios);
            timer.expires_from_now(chrono::seconds(1));
            timer.async_wait(yield[ec]);
        }
        else {
            asio::spawn( ios
                       , [ s = move(socket)
                         , ipfs_cache_injector
                         ](asio::yield_context yield) mutable {
                             using Con = GenericConnectionImpl<tcp::socket>;

                             auto con = make_shared<Con>(move(s));
                             serve(con, ipfs_cache_injector, yield);

                             sys::error_code ec;
                             con->get_impl().shutdown(tcp::socket::shutdown_send, ec);
                         });
        }
    }
}

//------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    // Check command line arguments.
    if (argc != 2)
    {
        cerr <<
            "Usage: injector <address>:<port>\n"
            "Example:\n"
            "    injector 0.0.0.0:8080\n";

        return EXIT_FAILURE;
    }

    auto const injector_ep = util::parse_endpoint(argv[1]);

    // The io_service is required for all I/O
    asio::io_service ios;

    asio::spawn
        ( ios
        , [&](asio::yield_context yield) {
              start(ios , injector_ep, yield);
          });

    ios.run();

    return EXIT_SUCCESS;
}
