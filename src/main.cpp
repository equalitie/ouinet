#include "proxy_session.h"
#include "fail.h"

using namespace ouinet;
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
namespace http = boost::beast::http;    // from <boost/beast/http.hpp>

// Accepts incoming connections and launches the proxy sessions
class Listener : public std::enable_shared_from_this<Listener>
{
    tcp::acceptor _acceptor;
    tcp::socket _socket;

public:
    Listener( boost::asio::io_service& ios
            , tcp::endpoint endpoint)
        : _acceptor(ios)
        , _socket(ios)
    {
        boost::system::error_code ec;

        // Open the acceptor
        _acceptor.open(endpoint.protocol(), ec);
        if(ec) {
            fail(ec, "open");
            return;
        }

        // Bind to the server address
        _acceptor.bind(endpoint, ec);
        if(ec) {
            fail(ec, "bind");
            return;
        }

        // Start listening for connections
        _acceptor.listen(boost::asio::socket_base::max_connections, ec);
        if(ec) {
            fail(ec, "listen");
            return;
        }
    }

    // Start accepting incoming connections
    void run()
    {
        if(! _acceptor.is_open()) return;
        do_accept();
    }

private:
    void do_accept()
    {
        _acceptor.async_accept(
            _socket,
            [this, self = shared_from_this()](boost::system::error_code ec) {
                if (ec) {
                    fail(ec, "accept");
                    return;
                }

                std::make_shared<ProxySession>(std::move(_socket))->run();

                do_accept();
            });
    }
};

//------------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // Check command line arguments.
    if (argc != 3)
    {
        std::cerr <<
            "Usage: http-server-async <address> <port>\n" <<
            "Example:\n" <<
            "    http-server-async 0.0.0.0 8080\n";
        return EXIT_FAILURE;
    }

    auto const address = boost::asio::ip::address::from_string(argv[1]);
    auto const port = static_cast<unsigned short>(std::atoi(argv[2]));

    // The io_service is required for all I/O
    boost::asio::io_service ios;

    // Create and launch a listening port
    std::make_shared<Listener>(
        ios,
        tcp::endpoint{address, port})->run();

    ios.run();

    return EXIT_SUCCESS;
}
